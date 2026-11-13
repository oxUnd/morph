#include "sync/sync.h"
#include <limits.h>
#include "sync/db_backup.h"
#include "content_hash.h"
#include "nb/vec.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/random.h"
#include "nb/encoding.h"
#include "nb/log.h"
#include "nb/str.h"
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
	return nb_str_is_alnum(nb_str_from_cstr(token), "_") && strncmp(token, "restore_", 8) == 0;
}

static int restore_plan_valid(const char *source_dir, const struct morph_sync_restore_plan *plan)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_2 = NB_OK;

	char expected[PATH_MAX];
	char directory[PATH_MAX];

	if (!source_dir || !plan || !restore_token_valid(plan->token) ||
	    !restore_rel_valid(plan->path) ||
	    strlen(plan->expected_hash) != MORPH_SYNC_HASH_LEN - 1 ||
	    strlen(plan->target) > PATH_MAX - sizeof("-wal") ||
	    strlen(plan->rollback) > PATH_MAX - sizeof("-wal"))
		return 0;
	if (((file_status_1 = nb_path_concat(&file_value_1, source_dir, plan->path)) != NB_OK
		     ? file_status_1
	     : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), expected,
					       sizeof(expected))) == NB_ERR_RANGE
		     ? NB_ERR_NAME_TOO_LONG
		     : file_status_1) != 0 ||
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
	if (((file_status_2 = nb_path_concat(&file_value_2, source_dir, ".morph-sync/restore")) !=
			     NB_OK
		     ? file_status_2
	     : (file_status_2 = nb_str_copy_to(nb_buf_str(&file_value_2), directory,
					       sizeof(directory))) == NB_ERR_RANGE
		     ? NB_ERR_NAME_TOO_LONG
		     : file_status_2) != 0 ||
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
		return NB_ERR_INVAL;
	position = strstr(plan->journal, marker);
	if (!position)
		return NB_ERR_FORMAT;
	length = (size_t)(position - plan->journal);
	if (length == 0 || length >= PATH_MAX)
		return NB_ERR_FORMAT;
	memcpy(source, plan->journal, length);
	source[length] = '\0';
	if (!restore_plan_valid(source, plan))
		return NB_ERR_FORMAT;
	return 0;
}

static int restore_sync_parent(const char *path);

static int restore_sync_parent(const char *path)
{
	nb_buf parent NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = nb_path_dirname(&parent, path);

	return rc != 0 ? rc : nb_file_sync_dir(nb_buf_cstr(&parent), NULL);
}

static int restore_unlink_optional(const char *path)
{
	if (unlink(path) != 0 && errno != ENOENT)
		return nb_status_from_errno(errno, NULL);
	return 0;
}

static int restore_journal_write(const struct morph_sync_restore_plan *plan, const char *phase)
{
	cJSON *root;
	char *json;
	int rc = 0;

	root = cJSON_CreateObject();
	if (!root)
		return NB_ERR_NOMEM;
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
		return NB_ERR_NOMEM;
	}
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json)
		return NB_ERR_NOMEM;
	rc = nb_file_write_all_atomic(
		plan->journal, json, strlen(json),
		&(nb_file_write_options){.new_file_mode = 0600, .sync_dir = true}, NULL);
	free(json);
	return rc;
}

static int restore_plan_from_json(const char *journal, struct morph_sync_restore_plan *plan,
				  char phase[16])
{
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_3 NB_CLEANUP(nb_charp_cleanup) = NULL;

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

	text = (nb_file_read_all(journal, &file_value_3, 0, NULL) == NB_OK &&
				(file_copy_3 = (char *)malloc(nb_buf_len(&file_value_3) + 1)) !=
					NULL
			? (memcpy(file_copy_3, nb_buf_cstr(&file_value_3),
				  nb_buf_len(&file_value_3) + 1),
			   nb_charp_steal(&file_copy_3))
			: NULL);
	if (!text)
		return NB_ERR_IO;
	root = cJSON_Parse(text);
	free(text);
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		return NB_ERR_PARSE;
	}
	memset(plan, 0, sizeof(*plan));
	item = cJSON_GetObjectItem(root, "phase");
	if (!cJSON_IsString(item)) {
		cJSON_Delete(root);
		return NB_ERR_PARSE;
	}
	(void)nb_str_copy_to(
		nb_str_from_parts((item->valuestring), strnlen((item->valuestring), 15)), phase,
		(15 + 1));
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		item = cJSON_GetObjectItem(root, names[i]);
		if (!cJSON_IsString(item) || strlen(item->valuestring) >= sizes[i]) {
			cJSON_Delete(root);
			return NB_ERR_PARSE;
		}
		(void)nb_str_copy_to(nb_str_from_parts((item->valuestring),
						       strnlen((item->valuestring), sizes[i] - 1)),
				     values[i], sizes[i]);
	}
	cJSON_Delete(root);
	return 0;
}

static int restore_sidecar_path(const char *path, const char *suffix, char out[PATH_MAX])
{
	nb_buf value NB_CLEANUP(nb_buf_cleanup) = {0};

	if (!nb_buf_append_cstr(&value, path) || !nb_buf_append_cstr(&value, suffix))
		return nb_buf_status(&value);
	int rc = nb_str_copy_to(nb_buf_str(&value), out, PATH_MAX);
	return rc == NB_ERR_RANGE ? NB_ERR_NAME_TOO_LONG : rc;
}

int morph_sync_prepare_db_replace(const struct morph_sync_config *cfg, const char *snapshot_id,
				  struct morph_sync_restore_plan *plan)
{
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_4 = NB_OK;
	nb_buf file_value_5 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_5 = NB_OK;
	nb_buf file_value_6 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_6 = NB_OK;

	struct morph_sync_backup *items = NULL;
	struct morph_sync_status status;
	char journal_dir[PATH_MAX];
	int count = 0;
	int found = -1;
	int rc;

	if (!sync_db_backend_valid(cfg) || !snapshot_id || !plan)
		return NB_ERR_INVAL;
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
		return found < 0 ? NB_ERR_NOT_FOUND : NB_ERR_FORMAT;
	}
	(void)nb_str_copy_to(
		nb_str_from_parts((snapshot_id),
				  strnlen((snapshot_id), sizeof(plan->snapshot_id) - 1)),
		plan->snapshot_id, sizeof(plan->snapshot_id));
	(void)nb_str_copy_to(
		nb_str_from_parts((items[found].path),
				  strnlen((items[found].path), sizeof(plan->path) - 1)),
		plan->path, sizeof(plan->path));
	(void)nb_str_copy_to(
		nb_str_from_parts((items[found].hash),
				  strnlen((items[found].hash), sizeof(plan->expected_hash) - 1)),
		plan->expected_hash, sizeof(plan->expected_hash));
	morph_sync_backups_free(items);
	rc = ((file_status_4 = nb_path_concat(&file_value_4, cfg->source_dir, plan->path)) != NB_OK
		      ? file_status_4
	      : (file_status_4 = nb_str_copy_to(nb_buf_str(&file_value_4), plan->target,
						sizeof(plan->target))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_4);
	if (rc != 0)
		return rc;
	rc = ((file_status_5 = ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
				: !(nb_buf_clear(&file_value_5),
				    nb_buf_append_cstr(&file_value_5, "restore_"))
					? NB_ERR_NOMEM
					: nb_random_hex(&file_value_5, 16, NULL))) != NB_OK
		      ? file_status_5
	      : (file_status_5 = nb_str_copy_to(nb_buf_str(&file_value_5), plan->token,
						sizeof(plan->token))) == NB_ERR_RANGE
		      ? NB_ERR_NO_SPACE
		      : file_status_5);
	if (rc != 0)
		return rc;
	if (snprintf(plan->staging, sizeof(plan->staging), "%s.%s.tmp", plan->target,
		     plan->token) >= (int)sizeof(plan->staging) ||
	    snprintf(plan->rollback, sizeof(plan->rollback), "%s.%s.rollback", plan->target,
		     plan->token) >= (int)sizeof(plan->rollback))
		return NB_ERR_NAME_TOO_LONG;
	rc = ((file_status_6 = nb_path_concat(&file_value_6, cfg->source_dir,
					      ".morph-sync/restore")) != NB_OK
		      ? file_status_6
	      : (file_status_6 = nb_str_copy_to(nb_buf_str(&file_value_6), journal_dir,
						sizeof(journal_dir))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_6);
	if (rc == 0)
		rc = nb_file_mkdir(journal_dir, true, NULL);
	if (rc != 0)
		return rc;
	if (snprintf(plan->journal, sizeof(plan->journal), "%s/%s.json", journal_dir,
		     plan->token) >= (int)sizeof(plan->journal))
		return NB_ERR_NAME_TOO_LONG;
	if (!restore_plan_valid(cfg->source_dir, plan))
		return NB_ERR_FORMAT;
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

	int rc = restore_sidecar_path(from, suffix, source);
	if (rc == 0)
		rc = restore_sidecar_path(to, suffix, destination);
	if (rc != 0)
		return rc;
	if (nb_file_exists(source) && rename(source, destination) != 0)
		return nb_status_from_errno(errno, NULL);
	return 0;
}

int morph_sync_apply_db_replace(struct morph_sync_restore_plan *plan)
{
	int moved = 0;
	char actual_hash[MORPH_SYNC_HASH_LEN];
	char source[PATH_MAX];
	int rc;

	if (!plan)
		return NB_ERR_INVAL;
	rc = restore_plan_source(plan, source);
	if (rc != 0)
		return rc;
	if (!sync_db_quick_check(plan->staging))
		return NB_ERR_FORMAT;
	rc = sync_hash_file(plan->staging, actual_hash);
	if (rc != 0)
		return rc;
	if (strcmp(actual_hash, plan->expected_hash) != 0)
		return NB_ERR_FORMAT;
	rc = restore_journal_write(plan, "applying");
	if (rc != 0)
		return rc;
	if (nb_file_exists(plan->target)) {
		if (rename(plan->target, plan->rollback) != 0)
			return nb_status_from_errno(errno, NULL);
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
		rc = nb_status_from_errno(errno, NULL);
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
		return NB_ERR_FORMAT;
	rc = restore_unlink_optional(plan->rollback);
	if (rc == 0)
		rc = restore_sidecar_path(plan->rollback, "-wal", path);
	if (rc == 0)
		rc = restore_unlink_optional(path);
	if (rc == 0)
		rc = restore_sidecar_path(plan->rollback, "-shm", path);
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
		return NB_ERR_FORMAT;
	if (nb_file_exists(plan->rollback)) {
		rc = restore_unlink_optional(plan->target);
		if (rc != 0)
			return rc;
		if (rename(plan->rollback, plan->target) != 0)
			return nb_status_from_errno(errno, NULL);
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

static int restore_journals_process(struct nb_log *logger, const char *source_dir, int finalize)
{
	int directory_status_1 = NB_OK;

	nb_buf file_value_7 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_7 = NB_OK;
	nb_buf file_value_8 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_8 = NB_OK;

	char directory[PATH_MAX];
	char journal[PATH_MAX];
	nb_dir_listing names NB_CLEANUP(nb_dir_listing_destroy) = {0};
	int count = 0;
	int rc;

	rc = ((file_status_7 = nb_path_concat(&file_value_7, source_dir, ".morph-sync/restore")) !=
			      NB_OK
		      ? file_status_7
	      : (file_status_7 = nb_str_copy_to(nb_buf_str(&file_value_7), directory,
						sizeof(directory))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_7);
	if (rc != 0)
		return rc;
	rc = ((directory_status_1 = nb_dir_scan(
		       directory, NB_DIR_SCAN_FILES | NB_DIR_SCAN_SORT | NB_DIR_SCAN_FOLLOW_LINKS,
		       &names, NULL)) != NB_OK
		      ? directory_status_1
	      : names.names.len > INT_MAX ? NB_ERR_RANGE
					  : (count = (int)names.names.len, NB_OK));
	if (rc == NB_ERR_NOT_FOUND)
		return 0;
	for (int i = 0; rc == 0 && i < count; i++) {
		struct morph_sync_restore_plan plan;
		char phase[16] = {0};

		rc = ((file_status_8 = nb_path_concat(&file_value_8, directory,
						      nb_dir_listing_name(&names, (size_t)(i)))) !=
				      NB_OK
			      ? file_status_8
		      : (file_status_8 = nb_str_copy_to(nb_buf_str(&file_value_8), journal,
							sizeof(journal))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_8);
		if (rc == 0)
			rc = restore_plan_from_json(journal, &plan, phase);
		if (rc == 0 &&
		    (!restore_plan_valid(source_dir, &plan) || strcmp(journal, plan.journal) != 0))
			rc = NB_ERR_FORMAT;
		if (rc != 0) {
			nb_log_write(logger, NB_LOG_WARN, "ignoring invalid restore journal %s: %s",
				     journal,
				     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
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
	nb_dir_listing_destroy(&names);
	return rc;
}

int morph_sync_recover_db_replacements(struct nb_log *logger, const char *source_dir)
{
	if (!source_dir)
		return NB_ERR_INVAL;
	return restore_journals_process(logger, source_dir, 0);
}

int morph_sync_finalize_db_replacements(struct nb_log *logger, const char *source_dir)
{
	if (!source_dir)
		return NB_ERR_INVAL;
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
		return NB_ERR_INVAL;
	memset(plan, 0, sizeof(*plan));
	root = cJSON_Parse(json);
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		return NB_ERR_PARSE;
	}
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		cJSON *item = cJSON_GetObjectItem(root, names[i]);

		if (!cJSON_IsString(item) || strlen(item->valuestring) >= sizes[i]) {
			cJSON_Delete(root);
			return NB_ERR_PARSE;
		}
		(void)nb_str_copy_to(nb_str_from_parts((item->valuestring),
						       strnlen((item->valuestring), sizes[i] - 1)),
				     values[i], sizes[i]);
	}
	cJSON_Delete(root);
	return 0;
}
