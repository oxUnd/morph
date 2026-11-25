#include "runtime/sync.h"
#include <nb/str.h>
#include <nb/utf8.h>
#include "runtime/runtime_internal.h"

#include "errors.h"
#include <nb/cleanup.h>
#include <nb/file.h>
#include "persistence/orm/sync_manifest.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int runtime_sync_config_instance(
	struct runtime *runtime, const char *default_sync_dir,
	const struct morph_sync_backend *backend, struct morph_sync_config *out)
{
	char source[PATH_MAX];
	char *slash;
	if (!runtime || !out)
		return NB_ERR_INVAL;
	(void)nb_str_copy_to(
		nb_str_from_parts(runtime->context.config_path[0] ? runtime->context.config_path
								  : runtime->context.workdir,
				  nb_utf8_clamp_bytes(runtime->context.config_path[0]
							      ? runtime->context.config_path
							      : runtime->context.workdir,
						      sizeof(source) - 1)),
		source, (sizeof(source) - 1) + 1);
	slash = strrchr(source, '/');
	if (slash)
		*slash = '\0';
	else
		(void)nb_str_copy_to(
			nb_str_from_parts(".", nb_utf8_clamp_bytes(".", sizeof(source) - 1)),
			source, (sizeof(source) - 1) + 1);
	int rc = runtime_sync_config_from_config(&runtime->context.config.sync,
		source, default_sync_dir, backend, out);
	if (rc == 0)
		out->logger = runtime->options.logger;

	if (rc == 0 && db_backend(&runtime->context.database) == DB_BACKEND_POSTGRESQL) {
		int retained = 0;

		for (int i = 0; i < out->include_count; i++) {
			if (strcmp(out->include[i], "data.db") == 0 ||
			    strcmp(out->include[i], ".morph/data.db") == 0)
				continue;

			if (retained != i)
				memcpy(out->include[retained], out->include[i],
				       sizeof(out->include[retained]));

			retained++;
		}

		out->include_count = retained;
	}

	if (rc == 0 && runtime->context.tctx) {
		const char *output = tool_context_output_dir_borrow(runtime->context.tctx);
		if (output && output[0] == '/')
			(void)nb_str_copy_to(
				nb_str_from_parts(
					output,
					nb_utf8_clamp_bytes(output, sizeof(out->output_dir) - 1)),
				out->output_dir, (sizeof(out->output_dir) - 1) + 1);
	}
	return rc;
}

int runtime_sync_config_from_config(const struct config_sync *config,
				    const char *source_dir,
				    const char *default_sync_dir,
				    const struct morph_sync_backend *backend,
				    struct morph_sync_config *out)
{
	if (!config || !source_dir || !out)
		return NB_ERR_INVAL;
	memset(out, 0, sizeof(*out));
	out->enabled = config->enabled;
	out->interval_seconds = config->interval_seconds;
	out->retention_days = config->retention_days;
	(void)nb_str_copy_to(
		nb_str_from_parts(source_dir,
				  nb_utf8_clamp_bytes(source_dir, sizeof(out->source_dir) - 1)),
		out->source_dir, (sizeof(out->source_dir) - 1) + 1);
	(void)nb_str_copy_to(
		nb_str_from_parts(config->dir,
				  nb_utf8_clamp_bytes(config->dir, sizeof(out->sync_dir) - 1)),
		out->sync_dir, (sizeof(out->sync_dir) - 1) + 1);
	if (!out->sync_dir[0] && default_sync_dir)
		(void)nb_str_copy_to(
			nb_str_from_parts(
				default_sync_dir,
				nb_utf8_clamp_bytes(default_sync_dir, sizeof(out->sync_dir) - 1)),
			out->sync_dir, (sizeof(out->sync_dir) - 1) + 1);
	out->include_count = config->include_count;
	if (out->include_count > MORPH_SYNC_INCLUDE_MAX)
		out->include_count = MORPH_SYNC_INCLUDE_MAX;
	for (int i = 0; i < out->include_count; i++)
		(void)nb_str_copy_to(
			nb_str_from_parts(config->include[i],
					  nb_utf8_clamp_bytes(config->include[i],
							      MORPH_SYNC_INCLUDE_LEN_MAX - 1)),
			out->include[i], (MORPH_SYNC_INCLUDE_LEN_MAX - 1) + 1);
	out->remote_backend = backend;
	return 0;
}

static int runtime_sync_worker_start(struct morph_sync_worker *worker,
				     int *started,
				     const struct morph_sync_config *config)
{
	int rc;

	if (!worker || !started || !config)
		return NB_ERR_INVAL;
	if (*started)
		return 0;
	if (!config->enabled)
		return 0;
	if (!config->sync_dir[0])
		return NB_ERR_INVAL;
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
		return NB_ERR_INVAL;
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
		return NB_ERR_INVAL;
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
		return NB_ERR_INVAL;
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
	nb_buf metadata_dir_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int metadata_dir_status = NB_OK;
	nb_buf manifest_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int manifest_path_status = NB_OK;

	char metadata[PATH_MAX];
	int rc;
	if (!runtime || !path || size == 0 ||
	    !runtime->context.config.sync.dir[0])
		return NB_ERR_INVAL;
	rc = ((metadata_dir_status =
		       nb_path_concat(&metadata_dir_buffer, runtime->context.config.sync.dir,
				      ".morph-sync")) != NB_OK
		      ? metadata_dir_status
	      : (metadata_dir_status = nb_str_copy_to(nb_buf_str(&metadata_dir_buffer), metadata,
						      sizeof(metadata))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : metadata_dir_status);
	return rc == 0 ? ((manifest_path_status = nb_path_concat(&manifest_path_buffer, metadata,
								 "manifest.db")) != NB_OK
				  ? manifest_path_status
			  : (manifest_path_status = nb_str_copy_to(
				     nb_buf_str(&manifest_path_buffer), path, size)) == NB_ERR_RANGE
				  ? NB_ERR_NAME_TOO_LONG
				  : manifest_path_status)
		       : rc;
}

int runtime_sync_conflicts(struct runtime *runtime, struct runtime_sync_conflict **out, int *count)
{
	char path[PATH_MAX];

	if (!runtime || !out || !count)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	*count = 0;
	int rc = runtime_sync_manifest_path(runtime, path, sizeof(path));

	return rc == 0 ? manifest_conflicts_copy(path, out, count) : rc;
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
		return NB_ERR_INVAL;
	rc = runtime_sync_config_instance(runtime, NULL, NULL, &config);
	return rc == 0 ? morph_sync_restore_trash(&config, trash_id) : rc;
}

int runtime_sync_backups(struct runtime *runtime, const char *path,
			 struct morph_sync_backup **out, int *count)
{
	struct morph_sync_config config;
	int rc;

	if (!runtime || !out || !count)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	*count = 0;
	rc = runtime_sync_config_instance(runtime, NULL, NULL, &config);
	return rc == 0 ? morph_sync_backups(&config, path, out, count) : rc;
}

int runtime_sync_restore_db(struct runtime *runtime, const char *snapshot_id,
			    const char *destination)
{
	struct morph_sync_config config;
	int rc;

	if (!runtime || !snapshot_id || !destination)
		return NB_ERR_INVAL;
	rc = runtime_sync_config_instance(runtime, NULL, NULL, &config);
	return rc == 0 ? morph_sync_restore_db(&config, snapshot_id,
						destination) : rc;
}

int runtime_sync_prepare_db_replace(struct runtime *runtime,
				    const char *snapshot_id,
				    struct morph_sync_restore_plan *plan)
{
	struct morph_sync_config config;
	int was_running;
	int rc;

	if (!runtime || !snapshot_id || !plan)
		return NB_ERR_INVAL;
	was_running = runtime_sync_running(runtime);
	runtime_sync_stop_instance(runtime);
	rc = runtime_sync_config_instance(runtime, NULL, NULL, &config);
	if (rc == 0)
		rc = morph_sync_prepare_db_replace(&config, snapshot_id, plan);
	if (rc != 0 && was_running)
		(void)runtime_sync_start_instance(runtime, NULL, NULL);
	return rc;
}
