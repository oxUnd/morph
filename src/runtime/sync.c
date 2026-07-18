#include "runtime/sync.h"
#include "runtime/runtime_internal.h"

#include "util/error.h"
#include "util/file.h"
#include <sqlite3.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int runtime_sync_config_instance(
	struct runtime *runtime, const char *default_sync_dir,
	const struct morph_sync_backend *backend, struct morph_sync_config *out)
{
	char source[PATH_MAX];
	char *slash;
	if (!runtime || !out)
		return -EINVAL;
	strncpy(source, runtime->context.config_path[0]
		? runtime->context.config_path : runtime->context.workdir,
		sizeof(source) - 1);
	slash = strrchr(source, '/');
	if (slash)
		*slash = '\0';
	else
		strncpy(source, ".", sizeof(source) - 1);
	return runtime_sync_config_from_config(&runtime->context.config.sync,
		source, default_sync_dir, backend, out);
}

int runtime_sync_config_from_config(const struct config_sync *config,
				    const char *source_dir,
				    const char *default_sync_dir,
				    const struct morph_sync_backend *backend,
				    struct morph_sync_config *out)
{
	if (!config || !source_dir || !out)
		MORPH_RETURN(-EINVAL);
	memset(out, 0, sizeof(*out));
	out->enabled = config->enabled;
	out->interval_seconds = config->interval_seconds;
	out->retention_days = config->retention_days;
	strncpy(out->source_dir, source_dir, sizeof(out->source_dir) - 1);
	strncpy(out->sync_dir, config->dir, sizeof(out->sync_dir) - 1);
	if (!out->sync_dir[0] && default_sync_dir)
		strncpy(out->sync_dir, default_sync_dir,
			sizeof(out->sync_dir) - 1);
	out->include_count = config->include_count;
	if (out->include_count > MORPH_SYNC_INCLUDE_MAX)
		out->include_count = MORPH_SYNC_INCLUDE_MAX;
	for (int i = 0; i < out->include_count; i++)
		strncpy(out->include[i], config->include[i],
			MORPH_SYNC_INCLUDE_LEN_MAX - 1);
	out->remote_backend = backend;
	return 0;
}

static int runtime_sync_worker_start(struct morph_sync_worker *worker,
				     int *started,
				     const struct morph_sync_config *config)
{
	int rc;

	if (!worker || !started || !config)
		MORPH_RETURN(-EINVAL);
	if (*started)
		return 0;
	if (!config->enabled)
		return 0;
	if (!config->sync_dir[0])
		MORPH_RETURN(-EINVAL);
	rc = morph_sync_start(worker, config);
	if (rc == 0)
		*started = 1;
	return rc;
}

static void runtime_sync_worker_stop(struct morph_sync_worker *worker,
				     int *started)
{
	if (!worker || !started || !*started)
		return;
	morph_sync_stop(worker);
	*started = 0;
}

int runtime_sync_start_instance(struct runtime *runtime,
				const char *default_sync_dir,
				const struct morph_sync_backend *backend)
{
	struct morph_sync_config config;
	int rc;
	if (!runtime)
		return -EINVAL;
	if (!runtime->context.config.sync.enabled ||
	    runtime->context.config.sync.interval_seconds <= 0)
		return 0;
	rc = runtime_sync_config_instance(runtime, default_sync_dir,
		backend, &config);
	return rc == 0 ? runtime_sync_worker_start(&runtime->sync_worker,
		&runtime->sync_started, &config) : rc;
}

void runtime_sync_stop_instance(struct runtime *runtime)
{
	if (runtime)
		runtime_sync_worker_stop(&runtime->sync_worker,
					 &runtime->sync_started);
}

int runtime_sync_status_instance(struct runtime *runtime,
				 struct morph_sync_status *status)
{
	if (!runtime || !status)
		return -EINVAL;
	memset(status, 0, sizeof(*status));
	return runtime->sync_started
		? morph_sync_worker_status(&runtime->sync_worker, status) : 0;
}

int runtime_sync_now_instance(struct runtime *runtime,
			      const char *default_sync_dir,
			      const struct morph_sync_backend *backend,
			      struct morph_sync_status *status)
{
	struct morph_sync_config config;
	int rc;
	if (!runtime || !status)
		return -EINVAL;
	memset(status, 0, sizeof(*status));
	rc = runtime_sync_config_instance(runtime, default_sync_dir,
		backend, &config);
	return rc == 0 ? morph_sync_once(&config, status) : rc;
}

int runtime_sync_running(const struct runtime *runtime)
{
	return runtime ? runtime->sync_started : 0;
}

static int runtime_sync_manifest_path(struct runtime *runtime, char *path,
				      size_t size)
{
	char metadata[PATH_MAX];
	int rc;
	if (!runtime || !path || size == 0 ||
	    !runtime->context.config.sync.dir[0])
		return -EINVAL;
	rc = file_path_join(metadata, sizeof(metadata),
		runtime->context.config.sync.dir, ".morph-sync");
	return rc == 0
		? file_path_join(path, size, metadata, "manifest.db") : rc;
}

int runtime_sync_conflicts(struct runtime *runtime,
			   struct runtime_sync_conflict **out, int *count)
{
	struct runtime_sync_conflict *items = NULL;
	sqlite3_stmt *statement = NULL;
	sqlite3 *database = NULL;
	char path[PATH_MAX];
	int capacity = 0;
	int used = 0;
	int rc;
	if (!runtime || !out || !count)
		return -EINVAL;
	*out = NULL;
	*count = 0;
	rc = runtime_sync_manifest_path(runtime, path, sizeof(path));
	if (rc != 0)
		return rc;
	if (sqlite3_open_v2(path, &database, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		sqlite3_close(database);
		return -ENOENT;
	}
	rc = sqlite3_prepare_v2(database,
		"SELECT id,path,created_at FROM conflicts ORDER BY id DESC",
		-1, &statement, NULL);
	if (rc != SQLITE_OK) {
		sqlite3_close(database);
		return MORPH_ERR_DB;
	}
	while (sqlite3_step(statement) == SQLITE_ROW) {
		if (used == capacity) {
			int next = capacity ? capacity * 2 : 8;
			void *resized = realloc(items,
				(size_t)next * sizeof(*items));
			if (!resized) {
				rc = -ENOMEM;
				goto out;
			}
			items = resized;
			capacity = next;
		}
		memset(&items[used], 0, sizeof(items[used]));
		items[used].id = sqlite3_column_int64(statement, 0);
		const unsigned char *value = sqlite3_column_text(statement, 1);
		if (value)
			strncpy(items[used].path, (const char *)value,
				sizeof(items[used].path) - 1);
		items[used].created_at = sqlite3_column_int64(statement, 2);
		used++;
	}
	rc = 0;
out:
	sqlite3_finalize(statement);
	sqlite3_close(database);
	if (rc != 0) {
		free(items);
		return rc;
	}
	*out = items;
	*count = used;
	return 0;
}

void runtime_sync_conflicts_free(struct runtime_sync_conflict *items)
{
	free(items);
}

int runtime_sync_restore(struct runtime *runtime, int64_t trash_id)
{
	struct morph_sync_config config;
	int rc;
	if (!runtime || trash_id <= 0)
		return -EINVAL;
	rc = runtime_sync_config_instance(runtime, NULL, NULL, &config);
	return rc == 0 ? morph_sync_restore_trash(&config, trash_id) : rc;
}
