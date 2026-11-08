#include "sync/db_backup.h"
#include "util/array.h"
#include "util/error.h"
#include "util/cleanup.h"
#include "util/json.h"
#include "util/file.h"
#include "util/id.h"
#include "util/log.h"
#include "util/strmap.h"
#include "util/name.h"
#include "content_hash.h"
#include "cJSON.h"
#include <sqlite3.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DB_STORE_ROOT ".morph-sync/db/v1"
#define DB_OBJECT_DIR DB_STORE_ROOT "/objects"
#define DB_SNAPSHOT_DIR DB_STORE_ROOT "/snapshots"
#define DB_CHUNK_TARGET (1024U * 1024U)

struct snapshot_scan {
	const struct morph_sync_config *cfg;
	const char *path_filter;
	morph_array_t *items;
	int rc;
};

struct stored_snapshot {
	char name[MORPH_SYNC_SNAPSHOT_ID_MAX + 8];
	struct morph_sync_backup backup;
	int removed;
};

struct backup_staging {
	char snapshot[PATH_MAX];
	char chunk[PATH_MAX];
	int keep_snapshot;
};

static void backup_database_cleanup(sqlite3 **database)
{
	if (*database)
		sqlite3_close(*database);
	*database = NULL;
}

static void backup_stmt_cleanup(sqlite3_stmt **stmt)
{
	sqlite3_finalize(*stmt);
	*stmt = NULL;
}

static void backup_transfer_cleanup(sqlite3_backup **backup)
{
	if (*backup)
		(void)sqlite3_backup_finish(*backup);
	*backup = NULL;
}

static void backup_file_cleanup(FILE **file)
{
	if (*file)
		(void)fclose(*file);
	*file = NULL;
}

static void backup_bytes_cleanup(unsigned char **bytes)
{
	free(*bytes);
	*bytes = NULL;
}

static void backup_staging_cleanup(struct backup_staging *staging)
{
	if (!staging->keep_snapshot && staging->snapshot[0])
		(void)unlink(staging->snapshot);
	if (staging->chunk[0])
		(void)unlink(staging->chunk);
}

int sync_db_backend_valid(const struct morph_sync_config *cfg)
{
	return cfg && (!cfg->remote_backend ||
		(cfg->remote_backend->stat && cfg->remote_backend->list &&
		 cfg->remote_backend->copy_from_local &&
		 cfg->remote_backend->copy_to_local &&
		 cfg->remote_backend->delete_file));
}

static int ensure_parent(const char *path)
{
	char parent[PATH_MAX];
	char *slash;

	strncpy(parent, path, sizeof(parent) - 1);
	parent[sizeof(parent) - 1] = '\0';
	slash = strrchr(parent, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	return parent[0] ? file_ensure_dir(parent) : 0;
}

static int copy_plain(const char *source, const char *destination)
{
	unsigned char buffer[BUFSIZ];
	FILE *input;
	FILE *output;
	size_t read_size;

	input = fopen(source, "rb");
	if (!input)
		return morph_error_from_errno(errno);
	if (ensure_parent(destination) != 0) {
		fclose(input);
		return -EIO;
	}
	output = fopen(destination, "wb");
	if (!output) {
		fclose(input);
		return morph_error_from_errno(errno);
	}
	while ((read_size = fread(buffer, 1, sizeof(buffer), input)) > 0) {
		if (fwrite(buffer, 1, read_size, output) != read_size) {
			fclose(input);
			fclose(output);
			return -EIO;
		}
	}
	if (ferror(input) || fflush(output) != 0) {
		fclose(input);
		fclose(output);
		return -EIO;
	}
	fclose(input);
	if (fclose(output) != 0)
		return -EIO;
	return 0;
}

static int store_path(const struct morph_sync_config *cfg, const char *rel,
		      char out[PATH_MAX])
{
	return file_path_join(out, PATH_MAX, cfg->sync_dir, rel);
}

static int store_stat(const struct morph_sync_config *cfg, const char *rel,
		      struct morph_sync_backend_stat *result)
{
	char path[PATH_MAX];
	struct stat st;

	memset(result, 0, sizeof(*result));
	if (cfg->remote_backend)
		return cfg->remote_backend->stat(cfg->remote_backend->user_data,
					 rel, result);
	if (store_path(cfg, rel, path) != 0)
		return -ENAMETOOLONG;
	if (stat(path, &st) != 0)
		return errno == ENOENT ? 0 : -errno;
	result->exists = 1;
	result->is_dir = S_ISDIR(st.st_mode);
	result->size = (int64_t)st.st_size;
	result->mtime = (int64_t)st.st_mtime;
	return 0;
}

static int store_put(const struct morph_sync_config *cfg, const char *source,
		     const char *rel)
{
	char destination[PATH_MAX];
	char temporary[PATH_MAX];
	char random_id[64];
	int rc;

	if (cfg->remote_backend)
		return cfg->remote_backend->copy_from_local(
			cfg->remote_backend->user_data, source, rel, 0);
	rc = store_path(cfg, rel, destination);
	if (rc != 0)
		return rc;
	rc = ensure_parent(destination);
	if (rc != 0)
		return rc;
	rc = morph_random_id("tmp_", random_id, sizeof(random_id));
	if (rc != 0)
		return rc;
	if (snprintf(temporary, sizeof(temporary), "%s.%s", destination,
		     random_id) >= (int)sizeof(temporary))
		return -ENAMETOOLONG;
	rc = copy_plain(source, temporary);
	if (rc != 0)
		return rc;
	if (rename(temporary, destination) != 0) {
		int saved = errno;
		(void)unlink(temporary);
		return -saved;
	}
	return 0;
}

static int store_get(const struct morph_sync_config *cfg, const char *rel,
		     const char *destination)
{
	char source[PATH_MAX];

	if (cfg->remote_backend)
		return cfg->remote_backend->copy_to_local(
			cfg->remote_backend->user_data, rel, destination);
	if (store_path(cfg, rel, source) != 0)
		return -ENAMETOOLONG;
	return copy_plain(source, destination);
}

int sync_db_quick_check(const char *path)
{
	sqlite3_stmt *statement = NULL;
	sqlite3 *database = NULL;
	const unsigned char *text;
	int rc;

	rc = sqlite3_open_v2(path, &database, SQLITE_OPEN_READWRITE, NULL);
	if (rc != SQLITE_OK) {
		sqlite3_close(database);
		return 0;
	}
	rc = sqlite3_prepare_v2(database, "PRAGMA quick_check", -1,
				&statement, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_step(statement);
	text = rc == SQLITE_ROW ? sqlite3_column_text(statement, 0) : NULL;
	rc = text && strcmp((const char *)text, "ok") == 0;
	sqlite3_finalize(statement);
	sqlite3_close(database);
	return rc;
}

int sync_db_is_sqlite(const char *path)
{
	unsigned char header[16];
	sqlite3_stmt *statement = NULL;
	sqlite3 *database = NULL;
	FILE *file;
	int rc;

	if (!path)
		return 0;
	file = fopen(path, "rb");
	if (!file)
		return 0;
	if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
		fclose(file);
		return 0;
	}
	fclose(file);
	if (memcmp(header, "SQLite format 3\0", sizeof(header)) != 0)
		return 0;
	rc = sqlite3_open_v2(path, &database, SQLITE_OPEN_READONLY, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_prepare_v2(database, "PRAGMA schema_version", -1,
					&statement, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_step(statement);
	sqlite3_finalize(statement);
	sqlite3_close(database);
	return rc == SQLITE_ROW;
}

static int sqlite_snapshot(const char *source, const char *destination,
			   int *page_size)
{
	sqlite3 *output MORPH_CLEANUP(backup_database_cleanup) = NULL;
	sqlite3 *input MORPH_CLEANUP(backup_database_cleanup) = NULL;
	sqlite3_stmt *statement MORPH_CLEANUP(backup_stmt_cleanup) = NULL;
	sqlite3_backup *backup MORPH_CLEANUP(backup_transfer_cleanup) = NULL;
	int busy_retries = 0;
	int rc;

	rc = sqlite3_open_v2(source, &input, SQLITE_OPEN_READONLY, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	rc = sqlite3_open_v2(destination, &output,
			     SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	backup = sqlite3_backup_init(output, "main", input, "main");
	if (!backup)
		return MORPH_ERR_DB;
	do {
		rc = sqlite3_backup_step(backup, 256);
		if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
			if (++busy_retries > 500) {
				rc = SQLITE_BUSY;
				break;
			}
			sqlite3_sleep(10);
		} else {
			busy_retries = 0;
		}
	} while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);
	if (sqlite3_backup_finish(backup) != SQLITE_OK || rc != SQLITE_DONE) {
		backup = NULL;
		return MORPH_ERR_DB;
	}
	backup = NULL;
	rc = sqlite3_exec(output, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	rc = sqlite3_prepare_v2(output, "PRAGMA page_size", -1,
				&statement, NULL);
	if (rc == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW)
		*page_size = sqlite3_column_int(statement, 0);
	else
		rc = SQLITE_ERROR;

	return rc == SQLITE_OK && *page_size > 0 ? 0 : MORPH_ERR_DB;
}

static int device_id_get(const struct morph_sync_config *cfg,
			 char out[MORPH_SYNC_DEVICE_ID_MAX])
{
	char directory[PATH_MAX];
	char path[PATH_MAX];
	char *stored;
	int rc;

	rc = file_path_join(directory, sizeof(directory), cfg->source_dir,
			    ".morph-sync");
	if (rc != 0)
		return rc;
	rc = file_ensure_dir(directory);
	if (rc != 0)
		return rc;
	rc = file_path_join(path, sizeof(path), directory, "device-id");
	if (rc != 0)
		return rc;
	stored = file_read_all_copy(path, NULL);
	if (stored) {
		stored[strcspn(stored, "\r\n")] = '\0';
		if (stored[0]) {
			strncpy(out, stored, MORPH_SYNC_DEVICE_ID_MAX - 1);
			free(stored);
			return 0;
		}
		free(stored);
	}
	rc = morph_random_id("dev_", out, MORPH_SYNC_DEVICE_ID_MAX);
	if (rc != 0)
		return rc;
	return file_write_all(path, out, strlen(out));
}

static int db_cursor_path(const struct morph_sync_config *cfg,
			  const char *database_key, char out[PATH_MAX])
{
	char directory[PATH_MAX];
	int rc;

	rc = file_path_join(directory, sizeof(directory), cfg->source_dir,
			    ".morph-sync/db-state");
	if (rc != 0)
		return rc;
	rc = file_ensure_dir(directory);
	if (rc != 0)
		return rc;
	return file_path_join(out, PATH_MAX, directory, database_key);
}

static int db_cursor_matches(const struct morph_sync_config *cfg,
			     const char *database_key, const char *device,
			     const char *database_hash)
{
	char path[PATH_MAX];
	char expected[MORPH_SYNC_DEVICE_ID_MAX + MORPH_SYNC_HASH_LEN + 2];
	char *stored;
	int matches;

	if (db_cursor_path(cfg, database_key, path) != 0)
		return 0;
	stored = file_read_all_copy(path, NULL);
	if (!stored)
		return 0;
	snprintf(expected, sizeof(expected), "%s %s", device, database_hash);
	stored[strcspn(stored, "\r\n")] = '\0';
	matches = strcmp(stored, expected) == 0;
	free(stored);
	return matches;
}

static int db_cursor_put(const struct morph_sync_config *cfg,
			 const char *database_key, const char *device,
			 const char *database_hash)
{
	char path[PATH_MAX];
	char value[MORPH_SYNC_DEVICE_ID_MAX + MORPH_SYNC_HASH_LEN + 2];
	int rc;

	rc = db_cursor_path(cfg, database_key, path);
	if (rc != 0)
		return rc;
	if (snprintf(value, sizeof(value), "%s %s", device, database_hash) >=
	    (int)sizeof(value))
		return -ENAMETOOLONG;
	return file_write_all(path, value, strlen(value));
}

static int staging_path(const struct morph_sync_config *cfg, const char *suffix,
			char out[PATH_MAX])
{
	char directory[PATH_MAX];
	char random_id[64];
	int rc;

	out[0] = '\0';
	rc = file_path_join(directory, sizeof(directory), cfg->sync_dir,
			    ".morph-sync/staging");
	if (rc != 0)
		return rc;
	rc = file_ensure_dir(directory);
	if (rc != 0)
		return rc;
	rc = morph_random_id("db_", random_id, sizeof(random_id));
	if (rc != 0)
		return rc;
	int length = snprintf(out, PATH_MAX, "%s/%s%s", directory, random_id,
			      suffix ? suffix : "");
	if (length < 0 || length >= PATH_MAX) {
		out[0] = '\0';
		return -ENAMETOOLONG;
	}
	return 0;
}

static int json_add_string(cJSON *object, const char *name, const char *value)
{
	return cJSON_AddStringToObject(object, name, value) ? 0 : -ENOMEM;
}

/* Bundle display history into the core snapshot, never into the live database.
 * The core transcript remains authoritative if a UI writer was ahead/behind.
 * Readers validate message anchors before accepting this optional projection. */
static int snapshot_include_display(const struct morph_sync_config *cfg,
				    const char *rel, const char *snapshot)
{
	char source[PATH_MAX];
	struct backup_staging staging MORPH_CLEANUP(backup_staging_cleanup) = {0};
	struct stat st;
	sqlite3 *db MORPH_CLEANUP(backup_database_cleanup) = NULL;
	sqlite3_stmt *attach MORPH_CLEANUP(backup_stmt_cleanup) = NULL;
	int page_size = 0;
	int rc;

	if (strcmp(rel, "data.db") != 0)
		return 0;
	rc = file_path_join(source, sizeof(source), cfg->source_dir, "ui-history.db");
	if (rc != 0)
		return rc;
	if (stat(source, &st) != 0) {
		if (errno == ENOENT)
			return 0;
		return morph_error_from_errno(errno);
	}
	rc = staging_path(cfg, ".display.db", staging.snapshot);
	if (rc != 0)
		return rc;
	rc = sqlite_snapshot(source, staging.snapshot, &page_size);
	if (rc != 0)
		return rc;
	if (sqlite3_open_v2(snapshot, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK ||
	    sqlite3_prepare_v2(db, "ATTACH DATABASE ? AS display", -1,
			       &attach, NULL) != SQLITE_OK) {
		return MORPH_ERR_DB;
	}
	sqlite3_bind_text(attach, 1, staging.snapshot, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(attach) != SQLITE_DONE) {
		return MORPH_ERR_DB;
	}
	if (sqlite3_exec(db,
		"BEGIN IMMEDIATE;"
		"CREATE TEMP TABLE local_display AS "
		"SELECT session_id,turn_id,core_message_id,seq,type,content,"
		"attachments_json,structured_data,agent_ui_ir,hitl_verdict,"
		"created_at,updated_at FROM display.ui_messages;"
		"CREATE TABLE IF NOT EXISTS sync_ui_messages AS SELECT * FROM local_display WHERE 0;",
		NULL, NULL, NULL) != SQLITE_OK)
		rc = (MORPH_ERR_DB);
	if (rc == 0 && sqlite3_table_column_metadata(db, "display",
		"ui_session_identity", "identity", NULL, NULL, NULL, NULL, NULL) == SQLITE_OK &&
	    sqlite3_table_column_metadata(db, "main", "sessions", "display_id",
		NULL, NULL, NULL, NULL, NULL) == SQLITE_OK) {
		if (sqlite3_exec(db,
			"DELETE FROM local_display WHERE session_id IN ("
			"SELECT i.session_id FROM display.ui_session_identity i "
			"JOIN main.sessions s ON s.id=i.session_id "
			"WHERE i.identity<>s.display_id)", NULL, NULL, NULL) != SQLITE_OK)
			rc = (MORPH_ERR_DB);
	}

	if (rc == 0 && sqlite3_exec(db,
		"DELETE FROM sync_ui_messages WHERE session_id IN "
		"(SELECT DISTINCT session_id FROM local_display);"
		"INSERT INTO sync_ui_messages SELECT * FROM local_display;"
		"CREATE INDEX IF NOT EXISTS sync_ui_session_seq "
		"ON sync_ui_messages(session_id,seq); COMMIT", NULL, NULL, NULL) != SQLITE_OK)
		rc = (MORPH_ERR_DB);
	return rc;
}

int sync_db_backup_create_one(const struct morph_sync_config *cfg, const char *rel,
			      const char *path, struct morph_sync_status *status, int update_cursor)
{
	struct backup_staging staging MORPH_CLEANUP(backup_staging_cleanup) = {0};
	char object_rel[PATH_MAX];
	char manifest_rel[PATH_MAX];
	char database_hash[MORPH_SYNC_HASH_LEN];
	char database_key[MORPH_SYNC_HASH_LEN];
	char device[MORPH_SYNC_DEVICE_ID_MAX] = { 0 };
	char snapshot_id[MORPH_SYNC_SNAPSHOT_ID_MAX];
	char version_id[MORPH_SYNC_HASH_LEN];
	struct morph_sync_backend_stat remote_stat;
	cJSON *root MORPH_CLEANUP(morph_json_cleanup) = NULL;
	cJSON *chunks MORPH_CLEANUP(morph_json_cleanup) = NULL;
	unsigned char *buffer MORPH_CLEANUP(backup_bytes_cleanup) = NULL;
	char *json MORPH_CLEANUP(morph_json_text_cleanup) = NULL;
	FILE *snapshot MORPH_CLEANUP(backup_file_cleanup) = NULL;
	int page_size = 0;
	size_t chunk_size;
	int rc;

	if (!sync_db_backend_valid(cfg) || !rel || !path || !status)
		return -EINVAL;
	rc = staging_path(cfg, ".db", staging.snapshot);
	if (rc != 0)
		return rc;
	rc = sqlite_snapshot(path, staging.snapshot, &page_size);
	if (rc != 0)
		return rc;
	if (update_cursor) {
		rc = snapshot_include_display(cfg, rel, staging.snapshot);
		if (rc != 0)
			return rc;
	}
	if (!sync_db_quick_check(staging.snapshot)) {
		return MORPH_ERR_FORMAT;
	}
	rc = sync_hash_file(staging.snapshot, database_hash);
	if (rc != 0)
		return rc;
	sync_hash_bytes(rel, strlen(rel), database_key);
	rc = device_id_get(cfg, device);
	if (rc != 0)
		return rc;
	if (update_cursor && db_cursor_matches(cfg, database_key, device,
					       database_hash))
		return rc;
	if (update_cursor) {
		rc = morph_random_id("ver_", version_id, sizeof(version_id));
		if (rc != 0)
			return rc;
	} else {
		strncpy(version_id, database_hash, sizeof(version_id) - 1);
		version_id[sizeof(version_id) - 1] = '\0';
	}
	if (snprintf(snapshot_id, sizeof(snapshot_id), "%s-%s-%s",
		     database_key, device, version_id) >=
	    (int)sizeof(snapshot_id)) {
		return -ENAMETOOLONG;
	}
	if (snprintf(manifest_rel, sizeof(manifest_rel), "%s/%s.json",
		     DB_SNAPSHOT_DIR, snapshot_id) >= (int)sizeof(manifest_rel)) {
		return -ENAMETOOLONG;
	}
	rc = store_stat(cfg, manifest_rel, &remote_stat);
	if (rc != 0 || remote_stat.exists)
		return rc;

	root = cJSON_CreateObject();
	chunks = cJSON_CreateArray();
	if (!root || !chunks) {
		return -ENOMEM;
	}
	cJSON_AddNumberToObject(root, "version", 1);
	if (json_add_string(root, "snapshot_id", snapshot_id) != 0 ||
	    json_add_string(root, "path", rel) != 0 ||
	    json_add_string(root, "device_id", device) != 0 ||
	    json_add_string(root, "database_hash", database_hash) != 0) {
		return -ENOMEM;
	}
	cJSON_AddNumberToObject(root, "created_at", (double)time(NULL));
	{
		struct stat st;
		if (stat(staging.snapshot, &st) != 0) {
			return -errno;
		}
		cJSON_AddNumberToObject(root, "database_size", (double)st.st_size);
	}
	cJSON_AddNumberToObject(root, "page_size", page_size);
	chunk_size = DB_CHUNK_TARGET / (size_t)page_size * (size_t)page_size;
	if (chunk_size == 0)
		chunk_size = (size_t)page_size;
	cJSON_AddNumberToObject(root, "chunk_size", (double)chunk_size);
	rc = morph_json_object_take(root, "chunks", &chunks);
	if (rc != 0)
		return rc;
	buffer = malloc(chunk_size);
	if (!buffer) {
		return -ENOMEM;
	}
	snapshot = fopen(staging.snapshot, "rb");
	if (!snapshot) {
		return -errno;
	}
	rc = staging_path(cfg, ".chunk", staging.chunk);
	if (rc != 0)
		return rc;
	while (1) {
		char chunk_hash[MORPH_SYNC_HASH_LEN];
		cJSON *item;
		size_t amount = fread(buffer, 1, chunk_size, snapshot);
		if (amount == 0)
			break;
		sync_hash_bytes(buffer, amount, chunk_hash);
		if (snprintf(object_rel, sizeof(object_rel), "%s/%.2s/%s",
			     DB_OBJECT_DIR, chunk_hash, chunk_hash) >=
		    (int)sizeof(object_rel)) {
			return -ENAMETOOLONG;
		}
		rc = store_stat(cfg, object_rel, &remote_stat);
		if (rc != 0)
			return rc;
		if (!remote_stat.exists) {
			rc = file_write_all(staging.chunk, (const char *)buffer, amount);
			if (rc != 0)
				return rc;
			rc = store_put(cfg, staging.chunk, object_rel);
			if (rc != 0)
				return rc;
			status->db_chunks_uploaded++;
			status->db_bytes_uploaded += (int64_t)amount;
		} else {
			status->db_chunks_reused++;
		}
		item = cJSON_CreateObject();
		if (!item || !cJSON_AddStringToObject(item, "hash", chunk_hash) ||
		    !cJSON_AddNumberToObject(item, "size", (double)amount)) {
			cJSON_Delete(item);
			return -ENOMEM;
		}
		cJSON_AddItemToArray(cJSON_GetObjectItem(root, "chunks"), item);
	}
	if (ferror(snapshot)) {
		return -EIO;
	}
	json = cJSON_PrintUnformatted(root);
	if (!json) {
		return -ENOMEM;
	}
	rc = file_write_all(staging.chunk, json, strlen(json));
	if (rc == 0)
		rc = store_put(cfg, staging.chunk, manifest_rel);
	if (rc == 0 && update_cursor)
		rc = db_cursor_put(cfg, database_key, device, database_hash);
	if (rc == 0)
		status->db_snapshots++;

	return rc;
}

int sync_db_backup_create(const struct morph_sync_config *cfg,
			  const char *rel, const char *path,
			  struct morph_sync_status *status)
{
	char directory[PATH_MAX];
	char prefix[PATH_MAX];
	char conflict_path[PATH_MAX];
	char **names = NULL;
	char *slash;
	const char *base;
	int count = 0;
	int rc;

	rc = sync_db_backup_create_one(cfg, rel, path, status, 1);
	if (rc != 0)
		return rc;
	strncpy(directory, path, sizeof(directory) - 1);
	directory[sizeof(directory) - 1] = '\0';
	slash = strrchr(directory, '/');
	if (slash) {
		*slash = '\0';
		base = slash + 1;
	} else {
		strncpy(directory, ".", sizeof(directory) - 1);
		base = path;
	}
	if (snprintf(prefix, sizeof(prefix), "%s.conflict.", base) >=
	    (int)sizeof(prefix))
		return -ENAMETOOLONG;
	rc = file_list_files_copy(directory, &names, &count);
	if (rc == -ENOENT)
		return 0;
	for (int i = 0; rc == 0 && i < count; i++) {
		if (strncmp(names[i], prefix, strlen(prefix)) != 0)
			continue;
		rc = file_path_join(conflict_path, sizeof(conflict_path),
				    directory, names[i]);
		if (rc == 0 && sync_db_is_sqlite(conflict_path))
			rc = sync_db_backup_create_one(cfg, rel, conflict_path, status, 0);
	}
	file_free_list(names, count);
	return rc;
}

static int snapshot_id_valid(const char *snapshot_id)
{
	return morph_name_valid(snapshot_id, "-_");
}

static int manifest_load(const struct morph_sync_config *cfg, const char *name,
			 cJSON **out)
{
	char rel[PATH_MAX];
	char local[PATH_MAX] = { 0 };
	char direct[PATH_MAX];
	char *text;
	int rc;

	if (snprintf(rel, sizeof(rel), "%s/%s", DB_SNAPSHOT_DIR, name) >=
	    (int)sizeof(rel))
		return -ENAMETOOLONG;
	if (cfg->remote_backend) {
		rc = staging_path(cfg, ".json", local);
		if (rc != 0)
			return rc;
		rc = store_get(cfg, rel, local);
		if (rc != 0)
			return rc;
		text = file_read_all_copy(local, NULL);
		(void)unlink(local);
	} else {
		rc = store_path(cfg, rel, direct);
		if (rc != 0)
			return rc;
		text = file_read_all_copy(direct, NULL);
	}
	if (!text)
		return -ENOENT;
	*out = cJSON_Parse(text);
	free(text);
	return *out ? 0 : MORPH_ERR_PARSE;
}

static int backup_from_json(cJSON *root, struct morph_sync_backup *backup)
{
	cJSON *snapshot_id = cJSON_GetObjectItem(root, "snapshot_id");
	cJSON *path = cJSON_GetObjectItem(root, "path");
	cJSON *device = cJSON_GetObjectItem(root, "device_id");
	cJSON *hash = cJSON_GetObjectItem(root, "database_hash");
	cJSON *created = cJSON_GetObjectItem(root, "created_at");
	cJSON *size = cJSON_GetObjectItem(root, "database_size");

	if (!cJSON_IsString(snapshot_id) || !cJSON_IsString(path) ||
	    !cJSON_IsString(device) || !cJSON_IsString(hash) ||
	    !cJSON_IsNumber(created) || !cJSON_IsNumber(size))
		return MORPH_ERR_PARSE;
	memset(backup, 0, sizeof(*backup));
	strncpy(backup->snapshot_id, snapshot_id->valuestring,
		sizeof(backup->snapshot_id) - 1);
	strncpy(backup->path, path->valuestring, sizeof(backup->path) - 1);
	strncpy(backup->device_id, device->valuestring,
		sizeof(backup->device_id) - 1);
	strncpy(backup->hash, hash->valuestring, sizeof(backup->hash) - 1);
	backup->created_at = (int64_t)created->valuedouble;
	backup->size = (int64_t)size->valuedouble;
	return 0;
}

static int snapshot_scan_name(struct snapshot_scan *scan, const char *name)
{
	struct morph_sync_backup *backup;
	cJSON *root = NULL;
	size_t length;
	int rc;

	length = strlen(name);
	if (length < 6 || strcmp(name + length - 5, ".json") != 0)
		return 0;
	rc = manifest_load(scan->cfg, name, &root);
	if (rc != 0)
		return rc;
	backup = morph_array_push_borrow(scan->items);
	if (!backup) {
		cJSON_Delete(root);
		return -ENOMEM;
	}
	rc = backup_from_json(root, backup);
	cJSON_Delete(root);
	if (rc != 0) {
		scan->items->nelts--;
		return rc;
	}
	if (scan->path_filter && scan->path_filter[0] &&
	    strcmp(scan->path_filter, backup->path) != 0)
		scan->items->nelts--;
	return 0;
}

static int snapshot_remote_cb(const char *name, int is_dir, void *user_data)
{
	struct snapshot_scan *scan = user_data;

	if (!scan || scan->rc != 0 || is_dir)
		return scan ? scan->rc : -EINVAL;
	scan->rc = snapshot_scan_name(scan, name);
	return scan->rc;
}

static int backup_compare(const void *left, const void *right)
{
	const struct morph_sync_backup *a = left;
	const struct morph_sync_backup *b = right;

	if (a->created_at != b->created_at)
		return a->created_at < b->created_at ? 1 : -1;
	return strcmp(a->snapshot_id, b->snapshot_id);
}

int morph_sync_backups(const struct morph_sync_config *cfg,
			const char *path, struct morph_sync_backup **out,
			int *count)
{
	struct snapshot_scan scan;
	morph_array_t items;
	char directory[PATH_MAX];
	char **names = NULL;
	int name_count = 0;
	int rc;

	if (!sync_db_backend_valid(cfg) || !out || !count)
		return -EINVAL;
	*out = NULL;
	*count = 0;
	rc = morph_array_init(&items, 8, sizeof(struct morph_sync_backup));
	if (rc != 0)
		return rc;
	memset(&scan, 0, sizeof(scan));
	scan.cfg = cfg;
	scan.path_filter = path;
	scan.items = &items;
	if (cfg->remote_backend) {
		rc = cfg->remote_backend->list(cfg->remote_backend->user_data,
			DB_SNAPSHOT_DIR, snapshot_remote_cb, &scan);
		if (rc == 0)
			rc = scan.rc;
	} else {
		rc = store_path(cfg, DB_SNAPSHOT_DIR, directory);
		if (rc == 0)
			rc = file_list_files_copy(directory, &names, &name_count);
		if (rc == -ENOENT)
			rc = 0;
		for (int i = 0; rc == 0 && i < name_count; i++)
			rc = snapshot_scan_name(&scan, names[i]);
		file_free_list(names, name_count);
	}
	if (rc != 0) {
		morph_array_cleanup(&items);
		return rc;
	}
	if (items.nelts > 1)
		qsort(items.elts, items.nelts, sizeof(struct morph_sync_backup),
		      backup_compare);
	*out = items.elts;
	*count = (int)items.nelts;
	return 0;
}

void morph_sync_backups_free(struct morph_sync_backup *items)
{
	free(items);
}

static int restore_chunk(const struct morph_sync_config *cfg, cJSON *item,
			 FILE *output, const char *temporary_chunk)
{
	char object_rel[PATH_MAX];
	char actual_hash[MORPH_SYNC_HASH_LEN];
	cJSON *hash = cJSON_GetObjectItem(item, "hash");
	cJSON *size = cJSON_GetObjectItem(item, "size");
	char *data;
	size_t data_size;
	int rc;

	if (!cJSON_IsString(hash) || strlen(hash->valuestring) != 64 ||
	    !cJSON_IsNumber(size) || size->valuedouble < 0)
		return MORPH_ERR_PARSE;
	if (snprintf(object_rel, sizeof(object_rel), "%s/%.2s/%s",
		     DB_OBJECT_DIR, hash->valuestring, hash->valuestring) >=
	    (int)sizeof(object_rel))
		return -ENAMETOOLONG;
	rc = store_get(cfg, object_rel, temporary_chunk);
	if (rc != 0)
		return rc;
	data = file_read_all_copy(temporary_chunk, &data_size);
	(void)unlink(temporary_chunk);
	if (!data)
		return -EIO;
	sync_hash_bytes(data, data_size, actual_hash);
	if (data_size != (size_t)size->valuedouble ||
	    strcmp(actual_hash, hash->valuestring) != 0) {
		log_err(cfg ? cfg->logger : NULL, "database backup chunk failed verification: %s",
			hash->valuestring);
		free(data);
		return MORPH_ERR_FORMAT;
	}
	if (fwrite(data, 1, data_size, output) != data_size) {
		free(data);
		return -EIO;
	}
	free(data);
	return 0;
}

int morph_sync_restore_db(const struct morph_sync_config *cfg,
			  const char *snapshot_id, const char *destination)
{
	char manifest_name[MORPH_SYNC_SNAPSHOT_ID_MAX + 8];
	struct backup_staging staging MORPH_CLEANUP(backup_staging_cleanup) = {0};
	char actual_hash[MORPH_SYNC_HASH_LEN];
	cJSON *root MORPH_CLEANUP(morph_json_cleanup) = NULL;
	cJSON *chunks;
	cJSON *expected_hash;
	cJSON *expected_size;
	struct stat st;
	FILE *output MORPH_CLEANUP(backup_file_cleanup) = NULL;
	int rc;

	if (!sync_db_backend_valid(cfg) || !snapshot_id_valid(snapshot_id) || !destination ||
	    !destination[0])
		return -EINVAL;
	if (file_exists(destination))
		return -EEXIST;
	if (snprintf(manifest_name, sizeof(manifest_name), "%s.json",
		     snapshot_id) >= (int)sizeof(manifest_name))
		return -ENAMETOOLONG;
	rc = manifest_load(cfg, manifest_name, &root);
	if (rc != 0)
		return rc;
	chunks = cJSON_GetObjectItem(root, "chunks");
	expected_hash = cJSON_GetObjectItem(root, "database_hash");
	expected_size = cJSON_GetObjectItem(root, "database_size");
	if (!cJSON_IsArray(chunks) || !cJSON_IsString(expected_hash) ||
	    !cJSON_IsNumber(expected_size)) {
		return MORPH_ERR_PARSE;
	}
	rc = ensure_parent(destination);
	if (rc != 0)
		return rc;
	int path_length = snprintf(staging.snapshot, sizeof(staging.snapshot),
				   "%s.restore.tmp.%u", destination, (unsigned)getpid());
	if (path_length < 0 || path_length >= (int)sizeof(staging.snapshot)) {
		staging.snapshot[0] = '\0';
		return -ENAMETOOLONG;
	}
	rc = staging_path(cfg, ".chunk", staging.chunk);
	if (rc != 0)
		return rc;
	output = fopen(staging.snapshot, "wb");
	if (!output) {
		return -errno;
	}
	cJSON *item;
	cJSON_ArrayForEach(item, chunks) {
		rc = restore_chunk(cfg, item, output, staging.chunk);
		if (rc != 0)
			return rc;
	}
	int flush_rc = fflush(output);
	int close_rc = fclose(output);
	output = NULL;
	if (flush_rc != 0 || close_rc != 0)
		return -EIO;
	if (stat(staging.snapshot, &st) != 0 ||
	    (int64_t)st.st_size != (int64_t)expected_size->valuedouble) {
		log_err(cfg ? cfg->logger : NULL, "database restore size mismatch for %s",
			snapshot_id);
		return MORPH_ERR_FORMAT;
	}
	rc = sync_hash_file(staging.snapshot, actual_hash);
	if (rc != 0)
		return rc;
	if (strcmp(actual_hash, expected_hash->valuestring) != 0 ||
	    !sync_db_quick_check(staging.snapshot)) {
		log_err(cfg ? cfg->logger : NULL, "database restore integrity check failed for %s",
			snapshot_id);
		return MORPH_ERR_FORMAT;
	}
	if (rename(staging.snapshot, destination) != 0) {
		return -errno;
	}
	staging.keep_snapshot = 1;
	return 0;
}

static int store_delete(const struct morph_sync_config *cfg, const char *rel)
{
	char path[PATH_MAX];

	if (cfg->remote_backend)
		return cfg->remote_backend->delete_file(
			cfg->remote_backend->user_data, rel);
	if (store_path(cfg, rel, path) != 0)
		return -ENAMETOOLONG;
	if (unlink(path) != 0 && errno != ENOENT)
		return morph_error_from_errno(errno);
	return 0;
}

struct retention_scan {
	const struct morph_sync_config *cfg;
	morph_array_t *snapshots;
	int rc;
};

static int retention_add_name(struct retention_scan *scan, const char *name)
{
	struct stored_snapshot *stored;
	cJSON *root = NULL;
	size_t length = strlen(name);
	int rc;

	if (length < 6 || strcmp(name + length - 5, ".json") != 0)
		return 0;
	rc = manifest_load(scan->cfg, name, &root);
	if (rc != 0)
		return rc;
	stored = morph_array_push_borrow(scan->snapshots);
	if (!stored) {
		cJSON_Delete(root);
		return -ENOMEM;
	}
	memset(stored, 0, sizeof(*stored));
	strncpy(stored->name, name, sizeof(stored->name) - 1);
	rc = backup_from_json(root, &stored->backup);
	cJSON_Delete(root);
	if (rc != 0)
		scan->snapshots->nelts--;
	return rc;
}

static int retention_remote_cb(const char *name, int is_dir, void *user_data)
{
	struct retention_scan *scan = user_data;

	if (!scan || scan->rc != 0 || is_dir)
		return scan ? scan->rc : -EINVAL;
	scan->rc = retention_add_name(scan, name);
	return scan->rc;
}

static int retention_collect(const struct morph_sync_config *cfg,
			     morph_array_t *snapshots)
{
	struct retention_scan scan;
	char directory[PATH_MAX];
	char **names = NULL;
	int count = 0;
	int rc;

	memset(&scan, 0, sizeof(scan));
	scan.cfg = cfg;
	scan.snapshots = snapshots;
	if (cfg->remote_backend) {
		rc = cfg->remote_backend->list(cfg->remote_backend->user_data,
			DB_SNAPSHOT_DIR, retention_remote_cb, &scan);
		return rc == 0 ? scan.rc : rc;
	}
	rc = store_path(cfg, DB_SNAPSHOT_DIR, directory);
	if (rc != 0)
		return rc;
	rc = file_list_files_copy(directory, &names, &count);
	if (rc == -ENOENT)
		return 0;
	for (int i = 0; rc == 0 && i < count; i++)
		rc = retention_add_name(&scan, names[i]);
	file_free_list(names, count);
	return rc;
}

static int snapshot_is_latest(const struct stored_snapshot *items, size_t count,
			      size_t candidate)
{
	for (size_t i = 0; i < count; i++) {
		if (i == candidate)
			continue;
		if (strcmp(items[i].backup.path,
			   items[candidate].backup.path) == 0 &&
		    strcmp(items[i].backup.device_id,
			   items[candidate].backup.device_id) == 0 &&
		    (items[i].backup.created_at >
			     items[candidate].backup.created_at ||
		     (items[i].backup.created_at ==
			      items[candidate].backup.created_at &&
		      strcmp(items[i].backup.snapshot_id,
			     items[candidate].backup.snapshot_id) > 0)))
			return 0;
	}
	return 1;
}

static int live_chunks_add(const struct morph_sync_config *cfg,
			   const char *manifest_name, morph_strmap_t *live)
{
	cJSON *root = NULL;
	cJSON *chunks;
	cJSON *item;
	int rc;

	rc = manifest_load(cfg, manifest_name, &root);
	if (rc != 0)
		return rc;
	chunks = cJSON_GetObjectItem(root, "chunks");
	if (!cJSON_IsArray(chunks)) {
		cJSON_Delete(root);
		return MORPH_ERR_PARSE;
	}
	cJSON_ArrayForEach(item, chunks) {
		cJSON *hash = cJSON_GetObjectItem(item, "hash");
		if (!cJSON_IsString(hash)) {
			rc = MORPH_ERR_PARSE;
			break;
		}
		rc = morph_strmap_set_borrow(live, hash->valuestring, (void *)(uintptr_t)1);
		if (rc != 0)
			break;
	}
	cJSON_Delete(root);
	return rc;
}

struct object_gc {
	const struct morph_sync_config *cfg;
	morph_strmap_t *live;
	int64_t cutoff;
	char directory[PATH_MAX];
	int rc;
};

static int object_gc_one(struct object_gc *gc, const char *name)
{
	struct morph_sync_backend_stat st;
	char rel[PATH_MAX];
	int rc;

	if (morph_strmap_contains(gc->live, name))
		return 0;
	if (snprintf(rel, sizeof(rel), "%s/%s", gc->directory, name) >=
	    (int)sizeof(rel))
		return -ENAMETOOLONG;
	rc = store_stat(gc->cfg, rel, &st);
	if (rc != 0)
		return rc;
	if (st.exists && !st.is_dir && st.mtime < gc->cutoff)
		return store_delete(gc->cfg, rel);
	return 0;
}

static int object_file_cb(const char *name, int is_dir, void *user_data)
{
	struct object_gc *gc = user_data;

	if (!gc || gc->rc != 0 || is_dir)
		return gc ? gc->rc : -EINVAL;
	gc->rc = object_gc_one(gc, name);
	return gc->rc;
}

static int object_prefix_cb(const char *name, int is_dir, void *user_data)
{
	struct object_gc *gc = user_data;
	struct object_gc child;

	if (!gc || gc->rc != 0 || !is_dir)
		return gc ? gc->rc : -EINVAL;
	child = *gc;
	if (snprintf(child.directory, sizeof(child.directory), "%s/%s",
		     DB_OBJECT_DIR, name) >= (int)sizeof(child.directory))
		return -ENAMETOOLONG;
	gc->rc = gc->cfg->remote_backend->list(
		gc->cfg->remote_backend->user_data, child.directory,
		object_file_cb, &child);
	if (gc->rc == 0)
		gc->rc = child.rc;
	return gc->rc;
}

static int object_gc_local(struct object_gc *gc)
{
	char root[PATH_MAX];
	char **prefixes = NULL;
	int prefix_count = 0;
	int rc;

	rc = store_path(gc->cfg, DB_OBJECT_DIR, root);
	if (rc != 0)
		return rc;
	rc = file_list_dirs_copy(root, &prefixes, &prefix_count);
	if (rc == -ENOENT)
		return 0;
	for (int i = 0; rc == 0 && i < prefix_count; i++) {
		char directory[PATH_MAX];
		char **names = NULL;
		int count = 0;
		if (snprintf(gc->directory, sizeof(gc->directory), "%s/%s",
			     DB_OBJECT_DIR, prefixes[i]) >=
		    (int)sizeof(gc->directory)) {
			rc = -ENAMETOOLONG;
			break;
		}
		rc = store_path(gc->cfg, gc->directory, directory);
		if (rc == 0)
			rc = file_list_files_copy(directory, &names, &count);
		for (int j = 0; rc == 0 && j < count; j++)
			rc = object_gc_one(gc, names[j]);
		file_free_list(names, count);
	}
	file_free_list(prefixes, prefix_count);
	return rc;
}

int sync_db_backup_cleanup(const struct morph_sync_config *cfg)
{
	morph_array_t snapshots;
	morph_strmap_t live;
	struct stored_snapshot *items;
	struct object_gc gc;
	int64_t cutoff;
	int rc;

	if (!sync_db_backend_valid(cfg))
		return -EINVAL;
	if (cfg->retention_days <= 0)
		return 0;
	cutoff = (int64_t)time(NULL) -
		((int64_t)cfg->retention_days * 24 * 60 * 60);
	rc = morph_array_init(&snapshots, 8, sizeof(struct stored_snapshot));
	if (rc != 0)
		return rc;
	rc = retention_collect(cfg, &snapshots);
	if (rc != 0) {
		morph_array_cleanup(&snapshots);
		return rc;
	}
	items = snapshots.elts;
	for (size_t i = 0; i < snapshots.nelts; i++) {
		char rel[PATH_MAX];
		if (items[i].backup.created_at >= cutoff ||
		    snapshot_is_latest(items, snapshots.nelts, i))
			continue;
		if (snprintf(rel, sizeof(rel), "%s/%s", DB_SNAPSHOT_DIR,
			     items[i].name) >= (int)sizeof(rel)) {
			rc = -ENAMETOOLONG;
			break;
		}
		rc = store_delete(cfg, rel);
		if (rc != 0)
			break;
		items[i].removed = 1;
	}
	if (rc == 0)
		rc = morph_strmap_init(&live, 64);
	if (rc != 0) {
		morph_array_cleanup(&snapshots);
		return rc;
	}
	for (size_t i = 0; rc == 0 && i < snapshots.nelts; i++) {
		if (!items[i].removed)
			rc = live_chunks_add(cfg, items[i].name, &live);
	}
	memset(&gc, 0, sizeof(gc));
	gc.cfg = cfg;
	gc.live = &live;
	gc.cutoff = cutoff;
	if (rc == 0 && cfg->remote_backend)
		rc = cfg->remote_backend->list(cfg->remote_backend->user_data,
			DB_OBJECT_DIR, object_prefix_cb, &gc);
	else if (rc == 0)
		rc = object_gc_local(&gc);
	if (rc == 0)
		rc = gc.rc;
	morph_strmap_cleanup(&live);
	morph_array_cleanup(&snapshots);
	return rc;
}
