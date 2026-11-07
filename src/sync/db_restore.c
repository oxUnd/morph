#include "sync/sync.h"
#include "sync/db_backup.h"
#include "content_hash.h"
#include "util/array.h"
#include "util/error.h"
#include "util/file.h"
#include "util/id.h"
#include "util/log.h"
#include "util/name.h"
#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int restore_rel_valid(const char *path)
{
	const char *part;

	if (!path || !path[0] || path[0] == '/')
		return 0;
	for (part = path; *part;) {
		const char *end = strchr(part, '/');
		size_t len = end ? (size_t)(end - part) : strlen(part);

		if (len == 0 || (len == 1 && part[0] == '.') ||
		    (len == 2 && part[0] == '.' && part[1] == '.'))
			return 0;
		part = end ? end + 1 : part + len;
	}
	return 1;
}

static int restore_token_valid(const char *token)
{
	return morph_name_valid(token, "_") && strncmp(token, "restore_", 8) == 0;
}

static int restore_plan_valid(const char *source_dir, const struct morph_sync_restore_plan *plan)
{
	char expected[PATH_MAX];
	char directory[PATH_MAX];

	if (!source_dir || !plan || !restore_token_valid(plan->token) ||
	    !restore_rel_valid(plan->path) ||
	    strlen(plan->expected_hash) != MORPH_SYNC_HASH_LEN - 1)
		return 0;
	if (file_path_join(expected, sizeof(expected), source_dir, plan->path) != 0 ||
	    strcmp(expected, plan->target) != 0)
		return 0;
	if (snprintf(expected, sizeof(expected), "%s.%s.tmp", plan->target, plan->token) >=
		    (int)sizeof(expected) ||
	    strcmp(expected, plan->staging) != 0)
		return 0;
	if (snprintf(expected, sizeof(expected), "%s.%s.rollback", plan->target, plan->token) >=
		    (int)sizeof(expected) ||
	    strcmp(expected, plan->rollback) != 0)
		return 0;
	if (file_path_join(directory, sizeof(directory), source_dir, ".morph-sync/restore") != 0 ||
	    snprintf(expected, sizeof(expected), "%s/%s.json", directory, plan->token) >=
		    (int)sizeof(expected) ||
	    strcmp(expected, plan->journal) != 0)
		return 0;
	return 1;
}

static int restore_plan_source(const struct morph_sync_restore_plan *plan, char source[PATH_MAX])
{
	const char marker[] = "/.morph-sync/restore/";
	const char *position;
	size_t length;

	if (!plan)
		return -EINVAL;
	position = strstr(plan->journal, marker);
	if (!position)
		return MORPH_ERR_FORMAT;
	length = (size_t)(position - plan->journal);
	if (length == 0 || length >= PATH_MAX)
		return MORPH_ERR_FORMAT;
	memcpy(source, plan->journal, length);
	source[length] = '\0';
	if (!restore_plan_valid(source, plan))
		return MORPH_ERR_FORMAT;
	return 0;
}

static int restore_sync_parent(const char *path);

static int restore_sync_parent(const char *path)
{
	char parent[PATH_MAX];
	char *slash;
	int fd;
	int rc = 0;

	if (!path || strlen(path) >= sizeof(parent))
		return -EINVAL;
	strncpy(parent, path, sizeof(parent) - 1);
	parent[sizeof(parent) - 1] = '\0';
	slash = strrchr(parent, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	fd = open(parent, O_RDONLY);
	if (fd < 0)
		return morph_error_from_errno(errno);
	if (fsync(fd) != 0 && errno != EINVAL && errno != EROFS)
		rc = -errno;
	if (close(fd) != 0 && rc == 0)
		rc = -errno;
	return rc;
}

static int restore_unlink_optional(const char *path)
{
	if (unlink(path) != 0 && errno != ENOENT)
		return morph_error_from_errno(errno);
	return 0;
}

static int restore_journal_write(const struct morph_sync_restore_plan *plan, const char *phase)
{
	cJSON *root;
	char *json;
	int rc = 0;

	root = cJSON_CreateObject();
	if (!root)
		return -ENOMEM;
	if (!cJSON_AddStringToObject(root, "phase", phase) ||
	    !cJSON_AddStringToObject(root, "token", plan->token) ||
	    !cJSON_AddStringToObject(root, "snapshot_id", plan->snapshot_id) ||
	    !cJSON_AddStringToObject(root, "path", plan->path) ||
	    !cJSON_AddStringToObject(root, "target", plan->target) ||
	    !cJSON_AddStringToObject(root, "staging", plan->staging) ||
	    !cJSON_AddStringToObject(root, "rollback", plan->rollback) ||
	    !cJSON_AddStringToObject(root, "journal", plan->journal) ||
	    !cJSON_AddStringToObject(root, "expected_hash", plan->expected_hash)) {
		cJSON_Delete(root);
		return -ENOMEM;
	}
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json)
		return -ENOMEM;
	rc = file_write_atomic(plan->journal, json, strlen(json));
	free(json);
	return rc;
}

static int restore_plan_from_json(const char *journal, struct morph_sync_restore_plan *plan,
				  char phase[16])
{
	char *text;
	cJSON *root;
	const char *names[] = {"token",	  "snapshot_id", "path",    "target",
			       "staging", "rollback",	 "journal", "expected_hash"};
	char *values[] = {plan->token,	 plan->snapshot_id, plan->path,	   plan->target,
			  plan->staging, plan->rollback,    plan->journal, plan->expected_hash};
	size_t sizes[] = {sizeof(plan->token),	 sizeof(plan->snapshot_id),  sizeof(plan->path),
			  sizeof(plan->target),	 sizeof(plan->staging),	     sizeof(plan->rollback),
			  sizeof(plan->journal), sizeof(plan->expected_hash)};
	cJSON *item;

	text = file_read_all_copy(journal, NULL);
	if (!text)
		return -EIO;
	root = cJSON_Parse(text);
	free(text);
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		return MORPH_ERR_PARSE;
	}
	memset(plan, 0, sizeof(*plan));
	item = cJSON_GetObjectItem(root, "phase");
	if (!cJSON_IsString(item)) {
		cJSON_Delete(root);
		return MORPH_ERR_PARSE;
	}
	strncpy(phase, item->valuestring, 15);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		item = cJSON_GetObjectItem(root, names[i]);
		if (!cJSON_IsString(item) || strlen(item->valuestring) >= sizes[i]) {
			cJSON_Delete(root);
			return MORPH_ERR_PARSE;
		}
		strncpy(values[i], item->valuestring, sizes[i] - 1);
	}
	cJSON_Delete(root);
	return 0;
}

static void restore_sidecar_path(const char *path, const char *suffix, char out[PATH_MAX])
{
	(void)snprintf(out, PATH_MAX, "%s%s", path, suffix);
}

int morph_sync_prepare_db_replace(const struct morph_sync_config *cfg, const char *snapshot_id,
				  struct morph_sync_restore_plan *plan)
{
	struct morph_sync_backup *items = NULL;
	struct morph_sync_status status;
	char journal_dir[PATH_MAX];
	int count = 0;
	int found = -1;
	int rc;

	if (!sync_db_backend_valid(cfg) || !snapshot_id || !plan)
		return -EINVAL;
	memset(plan, 0, sizeof(*plan));
	rc = morph_sync_backups(cfg, NULL, &items, &count);
	if (rc != 0)
		return rc;
	for (int i = 0; i < count; i++) {
		if (strcmp(items[i].snapshot_id, snapshot_id) == 0) {
			found = i;
			break;
		}
	}
	if (found < 0 || !restore_rel_valid(items[found].path)) {
		morph_sync_backups_free(items);
		return found < 0 ? -ENOENT : MORPH_ERR_FORMAT;
	}
	strncpy(plan->snapshot_id, snapshot_id, sizeof(plan->snapshot_id) - 1);
	strncpy(plan->path, items[found].path, sizeof(plan->path) - 1);
	strncpy(plan->expected_hash, items[found].hash, sizeof(plan->expected_hash) - 1);
	morph_sync_backups_free(items);
	rc = file_path_join(plan->target, sizeof(plan->target), cfg->source_dir, plan->path);
	if (rc != 0)
		return rc;
	rc = morph_random_id("restore_", plan->token, sizeof(plan->token));
	if (rc != 0)
		return rc;
	if (snprintf(plan->staging, sizeof(plan->staging), "%s.%s.tmp", plan->target,
		     plan->token) >= (int)sizeof(plan->staging) ||
	    snprintf(plan->rollback, sizeof(plan->rollback), "%s.%s.rollback", plan->target,
		     plan->token) >= (int)sizeof(plan->rollback))
		return -ENAMETOOLONG;
	rc = file_path_join(journal_dir, sizeof(journal_dir), cfg->source_dir,
			    ".morph-sync/restore");
	if (rc == 0)
		rc = file_ensure_dir(journal_dir);
	if (rc != 0)
		return rc;
	if (snprintf(plan->journal, sizeof(plan->journal), "%s/%s.json", journal_dir,
		     plan->token) >= (int)sizeof(plan->journal))
		return -ENAMETOOLONG;
	if (!restore_plan_valid(cfg->source_dir, plan))
		return MORPH_ERR_FORMAT;
	if (sync_db_is_sqlite(plan->target)) {
		memset(&status, 0, sizeof(status));
		rc = sync_db_backup_create_one(cfg, plan->path, plan->target, &status, 1);
		if (rc != 0)
			return rc;
	}
	rc = morph_sync_restore_db(cfg, snapshot_id, plan->staging);
	if (rc != 0)
		return rc;
	rc = restore_journal_write(plan, "prepared");
	if (rc != 0)
		(void)unlink(plan->staging);
	return rc;
}

static int restore_move_sidecar(const char *from, const char *to, const char *suffix)
{
	char source[PATH_MAX];
	char destination[PATH_MAX];

	restore_sidecar_path(from, suffix, source);
	restore_sidecar_path(to, suffix, destination);
	if (file_exists(source) && rename(source, destination) != 0)
		return morph_error_from_errno(errno);
	return 0;
}

int morph_sync_apply_db_replace(struct morph_sync_restore_plan *plan)
{
	int moved = 0;
	char actual_hash[MORPH_SYNC_HASH_LEN];
	char source[PATH_MAX];
	int rc;

	if (!plan)
		return -EINVAL;
	rc = restore_plan_source(plan, source);
	if (rc != 0)
		return rc;
	if (!sync_db_quick_check(plan->staging))
		return MORPH_ERR_FORMAT;
	rc = sync_hash_file(plan->staging, actual_hash);
	if (rc != 0)
		return rc;
	if (strcmp(actual_hash, plan->expected_hash) != 0)
		return MORPH_ERR_FORMAT;
	rc = restore_journal_write(plan, "applying");
	if (rc != 0)
		return rc;
	if (file_exists(plan->target)) {
		if (rename(plan->target, plan->rollback) != 0)
			return morph_error_from_errno(errno);
		moved = 1;
		rc = restore_move_sidecar(plan->target, plan->rollback, "-wal");
		if (rc == 0)
			rc = restore_move_sidecar(plan->target, plan->rollback, "-shm");
		if (rc != 0) {
			(void)rename(plan->rollback, plan->target);
			(void)restore_move_sidecar(plan->rollback, plan->target, "-wal");
			(void)restore_move_sidecar(plan->rollback, plan->target, "-shm");
			return rc;
		}
	}
	if (rename(plan->staging, plan->target) != 0) {
		rc = -errno;
		if (moved) {
			(void)rename(plan->rollback, plan->target);
			(void)restore_move_sidecar(plan->rollback, plan->target, "-wal");
			(void)restore_move_sidecar(plan->rollback, plan->target, "-shm");
		}
		return rc;
	}
	rc = restore_sync_parent(plan->target);
	if (rc != 0) {
		(void)morph_sync_rollback_db_replace(plan);
		return rc;
	}
	rc = restore_journal_write(plan, "applied");
	if (rc != 0)
		(void)morph_sync_rollback_db_replace(plan);
	return rc;
}

int morph_sync_commit_db_replace(struct morph_sync_restore_plan *plan)
{
	char path[PATH_MAX];
	char source[PATH_MAX];
	int rc;

	if (!plan || restore_plan_source(plan, source) != 0)
		return MORPH_ERR_FORMAT;
	rc = restore_unlink_optional(plan->rollback);
	restore_sidecar_path(plan->rollback, "-wal", path);
	if (rc == 0)
		rc = restore_unlink_optional(path);
	restore_sidecar_path(plan->rollback, "-shm", path);
	if (rc == 0)
		rc = restore_unlink_optional(path);
	if (rc == 0)
		rc = restore_unlink_optional(plan->staging);
	if (rc == 0)
		rc = restore_sync_parent(plan->target);
	if (rc == 0)
		rc = restore_unlink_optional(plan->journal);
	return rc;
}

int morph_sync_rollback_db_replace(struct morph_sync_restore_plan *plan)
{
	char source[PATH_MAX];
	int rc;

	if (!plan || restore_plan_source(plan, source) != 0)
		return MORPH_ERR_FORMAT;
	if (file_exists(plan->rollback)) {
		rc = restore_unlink_optional(plan->target);
		if (rc != 0)
			return rc;
		if (rename(plan->rollback, plan->target) != 0)
			return morph_error_from_errno(errno);
		rc = restore_move_sidecar(plan->rollback, plan->target, "-wal");
		if (rc != 0)
			return rc;
		rc = restore_move_sidecar(plan->rollback, plan->target, "-shm");
		if (rc != 0)
			return rc;
		rc = restore_sync_parent(plan->target);
		if (rc != 0)
			return rc;
	}
	rc = restore_unlink_optional(plan->staging);
	if (rc == 0)
		rc = restore_unlink_optional(plan->journal);
	return rc;
}

static int restore_journals_process(struct morph_log *logger, const char *source_dir, int finalize)
{
	char directory[PATH_MAX];
	char journal[PATH_MAX];
	char **names = NULL;
	int count = 0;
	int rc;

	rc = file_path_join(directory, sizeof(directory), source_dir, ".morph-sync/restore");
	if (rc != 0)
		return rc;
	rc = file_list_files_copy(directory, &names, &count);
	if (rc == -ENOENT)
		return 0;
	for (int i = 0; rc == 0 && i < count; i++) {
		struct morph_sync_restore_plan plan;
		char phase[16] = {0};

		rc = file_path_join(journal, sizeof(journal), directory, names[i]);
		if (rc == 0)
			rc = restore_plan_from_json(journal, &plan, phase);
		if (rc == 0 &&
		    (!restore_plan_valid(source_dir, &plan) || strcmp(journal, plan.journal) != 0))
			rc = MORPH_ERR_FORMAT;
		if (rc != 0) {
			log_warn(logger, "ignoring invalid restore journal %s: %s", journal,
				 morph_strerror(rc));
			rc = 0;
			continue;
		}
		if (strcmp(phase, "applied") == 0) {
			if (finalize)
				rc = morph_sync_commit_db_replace(&plan);
		} else if (!finalize) {
			rc = morph_sync_rollback_db_replace(&plan);
		}
	}
	file_free_list(names, count);
	return rc;
}

int morph_sync_recover_db_replacements(struct morph_log *logger, const char *source_dir)
{
	if (!source_dir)
		return -EINVAL;
	return restore_journals_process(logger, source_dir, 0);
}

int morph_sync_finalize_db_replacements(struct morph_log *logger, const char *source_dir)
{
	if (!source_dir)
		return -EINVAL;
	return restore_journals_process(logger, source_dir, 1);
}

char *morph_sync_restore_plan_to_json(const struct morph_sync_restore_plan *plan)
{
	cJSON *root;
	char *json;

	if (!plan)
		return NULL;
	root = cJSON_CreateObject();
	if (!root)
		return NULL;
	cJSON_AddStringToObject(root, "token", plan->token);
	cJSON_AddStringToObject(root, "snapshot_id", plan->snapshot_id);
	cJSON_AddStringToObject(root, "path", plan->path);
	cJSON_AddStringToObject(root, "target", plan->target);
	cJSON_AddStringToObject(root, "staging", plan->staging);
	cJSON_AddStringToObject(root, "rollback", plan->rollback);
	cJSON_AddStringToObject(root, "journal", plan->journal);
	cJSON_AddStringToObject(root, "expected_hash", plan->expected_hash);
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return json;
}

int morph_sync_restore_plan_from_json(const char *json, struct morph_sync_restore_plan *plan)
{
	cJSON *root;
	const char *names[] = {"token",	  "snapshot_id", "path",    "target",
			       "staging", "rollback",	 "journal", "expected_hash"};
	char *values[] = {plan ? plan->token : NULL,   plan ? plan->snapshot_id : NULL,
			  plan ? plan->path : NULL,    plan ? plan->target : NULL,
			  plan ? plan->staging : NULL, plan ? plan->rollback : NULL,
			  plan ? plan->journal : NULL, plan ? plan->expected_hash : NULL};
	size_t sizes[] = {plan ? sizeof(plan->token) : 0,   plan ? sizeof(plan->snapshot_id) : 0,
			  plan ? sizeof(plan->path) : 0,    plan ? sizeof(plan->target) : 0,
			  plan ? sizeof(plan->staging) : 0, plan ? sizeof(plan->rollback) : 0,
			  plan ? sizeof(plan->journal) : 0, plan ? sizeof(plan->expected_hash) : 0};

	if (!json || !plan)
		return -EINVAL;
	memset(plan, 0, sizeof(*plan));
	root = cJSON_Parse(json);
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		return MORPH_ERR_PARSE;
	}
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		cJSON *item = cJSON_GetObjectItem(root, names[i]);

		if (!cJSON_IsString(item) || strlen(item->valuestring) >= sizes[i]) {
			cJSON_Delete(root);
			return MORPH_ERR_PARSE;
		}
		strncpy(values[i], item->valuestring, sizes[i] - 1);
	}
	cJSON_Delete(root);
	return 0;
}
