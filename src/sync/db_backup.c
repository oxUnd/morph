#include "sync/db_backup.h"
#include "util/array.h"
#include "util/error.h"
#include "util/file.h"
#include "util/id.h"
#include "util/log.h"
#include "util/strmap.h"
#include "blake3.h"
#include "cJSON.h"
#include <sqlite3.h>
#include <ctype.h>
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

static int backend_valid(const struct morph_sync_config *cfg)
{
	return cfg && (!cfg->remote_backend ||
		(cfg->remote_backend->stat && cfg->remote_backend->list &&
		 cfg->remote_backend->copy_from_local &&
		 cfg->remote_backend->copy_to_local &&
		 cfg->remote_backend->delete_file));
}

static void hash_to_hex(const unsigned char hash[BLAKE3_OUT_LEN],
			char out[MORPH_SYNC_HASH_LEN])
{
	static const char hex[] = "0123456789abcdef";

	for (size_t i = 0; i < BLAKE3_OUT_LEN; i++) {
		out[i * 2] = hex[hash[i] >> 4];
		out[i * 2 + 1] = hex[hash[i] & 0x0f];
	}
	out[BLAKE3_OUT_LEN * 2] = '\0';
}

static void hash_bytes(const void *data, size_t len,
		       char out[MORPH_SYNC_HASH_LEN])
{
	unsigned char hash[BLAKE3_OUT_LEN];
	blake3_hasher hasher;

	blake3_hasher_init(&hasher);
	blake3_hasher_update(&hasher, data, len);
	blake3_hasher_finalize(&hasher, hash, sizeof(hash));
	hash_to_hex(hash, out);
}

static int hash_file(const char *path, char out[MORPH_SYNC_HASH_LEN])
{
	unsigned char buffer[BUFSIZ];
	unsigned char hash[BLAKE3_OUT_LEN];
	blake3_hasher hasher;
	FILE *file;
	size_t read_size;

	file = fopen(path, "rb");
	if (!file)
		MORPH_RETURN_ERRNO();
	blake3_hasher_init(&hasher);
	while ((read_size = fread(buffer, 1, sizeof(buffer), file)) > 0)
		blake3_hasher_update(&hasher, buffer, read_size);
	if (ferror(file)) {
		fclose(file);
		MORPH_RETURN(-EIO);
	}
	fclose(file);
	blake3_hasher_finalize(&hasher, hash, sizeof(hash));
	hash_to_hex(hash, out);
	return 0;
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
		MORPH_RETURN_ERRNO();
	if (ensure_parent(destination) != 0) {
		fclose(input);
		MORPH_RETURN(-EIO);
	}
	output = fopen(destination, "wb");
	if (!output) {
		fclose(input);
		MORPH_RETURN_ERRNO();
	}
	while ((read_size = fread(buffer, 1, sizeof(buffer), input)) > 0) {
		if (fwrite(buffer, 1, read_size, output) != read_size) {
			fclose(input);
			fclose(output);
			MORPH_RETURN(-EIO);
		}
	}
	if (ferror(input) || fflush(output) != 0) {
		fclose(input);
		fclose(output);
		MORPH_RETURN(-EIO);
	}
	fclose(input);
	if (fclose(output) != 0)
		MORPH_RETURN(-EIO);
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
		MORPH_RETURN(-ENAMETOOLONG);
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
		MORPH_RETURN(-ENAMETOOLONG);
	rc = copy_plain(source, temporary);
	if (rc != 0)
		return rc;
	if (rename(temporary, destination) != 0) {
		int saved = errno;
		(void)unlink(temporary);
		MORPH_RETURN(-saved);
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
		MORPH_RETURN(-ENAMETOOLONG);
	return copy_plain(source, destination);
}

static int quick_check(const char *path)
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
	sqlite3_backup *backup = NULL;
	sqlite3_stmt *statement = NULL;
	sqlite3 *input = NULL;
	sqlite3 *output = NULL;
	int busy_retries = 0;
	int rc;

	rc = sqlite3_open_v2(source, &input, SQLITE_OPEN_READONLY, NULL);
	if (rc != SQLITE_OK)
		goto out;
	rc = sqlite3_open_v2(destination, &output,
			     SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
	if (rc != SQLITE_OK)
		goto out;
	backup = sqlite3_backup_init(output, "main", input, "main");
	if (!backup) {
		rc = sqlite3_errcode(output);
		goto out;
	}
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
		rc = SQLITE_ERROR;
		goto out;
	}
	backup = NULL;
	rc = sqlite3_exec(output, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL);
	if (rc != SQLITE_OK)
		goto out;
	rc = sqlite3_prepare_v2(output, "PRAGMA page_size", -1,
				&statement, NULL);
	if (rc == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW)
		*page_size = sqlite3_column_int(statement, 0);
	else
		rc = SQLITE_ERROR;

out:
	if (backup)
		(void)sqlite3_backup_finish(backup);
	sqlite3_finalize(statement);
	sqlite3_close(input);
	sqlite3_close(output);
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
	stored = file_read_all(path, NULL);
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
	stored = file_read_all(path, NULL);
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
		MORPH_RETURN(-ENAMETOOLONG);
	return file_write_all(path, value, strlen(value));
}

static int staging_path(const struct morph_sync_config *cfg, const char *suffix,
			char out[PATH_MAX])
{
	char directory[PATH_MAX];
	char random_id[64];
	int rc;

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
	if (snprintf(out, PATH_MAX, "%s/%s%s", directory, random_id,
		     suffix ? suffix : "") >= PATH_MAX)
		MORPH_RETURN(-ENAMETOOLONG);
	return 0;
}

static int json_add_string(cJSON *object, const char *name, const char *value)
{
	return cJSON_AddStringToObject(object, name, value) ? 0 : -ENOMEM;
}

static int db_backup_create_one(const struct morph_sync_config *cfg,
				const char *rel, const char *path,
				struct morph_sync_status *status,
				int update_cursor)
{
	char snapshot_path[PATH_MAX];
	char chunk_path[PATH_MAX] = { 0 };
	char object_rel[PATH_MAX];
	char manifest_rel[PATH_MAX];
	char database_hash[MORPH_SYNC_HASH_LEN];
	char database_key[MORPH_SYNC_HASH_LEN];
	char device[MORPH_SYNC_DEVICE_ID_MAX] = { 0 };
	char snapshot_id[MORPH_SYNC_SNAPSHOT_ID_MAX];
	char version_id[MORPH_SYNC_HASH_LEN];
	struct morph_sync_backend_stat remote_stat;
	cJSON *root = NULL;
	cJSON *chunks = NULL;
	unsigned char *buffer = NULL;
	char *json = NULL;
	FILE *snapshot = NULL;
	int page_size = 0;
	size_t chunk_size;
	int rc;

	if (!backend_valid(cfg) || !rel || !path || !status)
		MORPH_RETURN(-EINVAL);
	rc = staging_path(cfg, ".db", snapshot_path);
	if (rc != 0)
		return rc;
	rc = sqlite_snapshot(path, snapshot_path, &page_size);
	if (rc != 0)
		goto out;
	if (!quick_check(snapshot_path)) {
		rc = MORPH_ERR_FORMAT;
		goto out;
	}
	rc = hash_file(snapshot_path, database_hash);
	if (rc != 0)
		goto out;
	hash_bytes(rel, strlen(rel), database_key);
	rc = device_id_get(cfg, device);
	if (rc != 0)
		goto out;
	if (update_cursor && db_cursor_matches(cfg, database_key, device,
					       database_hash))
		goto out;
	if (update_cursor) {
		rc = morph_random_id("ver_", version_id, sizeof(version_id));
		if (rc != 0)
			goto out;
	} else {
		strncpy(version_id, database_hash, sizeof(version_id) - 1);
		version_id[sizeof(version_id) - 1] = '\0';
	}
	if (snprintf(snapshot_id, sizeof(snapshot_id), "%s-%s-%s",
		     database_key, device, version_id) >=
	    (int)sizeof(snapshot_id)) {
		rc = -ENAMETOOLONG;
		goto out;
	}
	if (snprintf(manifest_rel, sizeof(manifest_rel), "%s/%s.json",
		     DB_SNAPSHOT_DIR, snapshot_id) >= (int)sizeof(manifest_rel)) {
		rc = -ENAMETOOLONG;
		goto out;
	}
	rc = store_stat(cfg, manifest_rel, &remote_stat);
	if (rc != 0 || remote_stat.exists)
		goto out;

	root = cJSON_CreateObject();
	chunks = cJSON_CreateArray();
	if (!root || !chunks) {
		rc = -ENOMEM;
		goto out;
	}
	cJSON_AddNumberToObject(root, "version", 1);
	if (json_add_string(root, "snapshot_id", snapshot_id) != 0 ||
	    json_add_string(root, "path", rel) != 0 ||
	    json_add_string(root, "device_id", device) != 0 ||
	    json_add_string(root, "database_hash", database_hash) != 0) {
		rc = -ENOMEM;
		goto out;
	}
	cJSON_AddNumberToObject(root, "created_at", (double)time(NULL));
	{
		struct stat st;
		if (stat(snapshot_path, &st) != 0) {
			rc = -errno;
			goto out;
		}
		cJSON_AddNumberToObject(root, "database_size", (double)st.st_size);
	}
	cJSON_AddNumberToObject(root, "page_size", page_size);
	chunk_size = DB_CHUNK_TARGET / (size_t)page_size * (size_t)page_size;
	if (chunk_size == 0)
		chunk_size = (size_t)page_size;
	cJSON_AddNumberToObject(root, "chunk_size", (double)chunk_size);
	cJSON_AddItemToObject(root, "chunks", chunks);
	chunks = NULL;
	buffer = malloc(chunk_size);
	if (!buffer) {
		rc = -ENOMEM;
		goto out;
	}
	snapshot = fopen(snapshot_path, "rb");
	if (!snapshot) {
		rc = -errno;
		goto out;
	}
	rc = staging_path(cfg, ".chunk", chunk_path);
	if (rc != 0)
		goto out;
	while (1) {
		char chunk_hash[MORPH_SYNC_HASH_LEN];
		cJSON *item;
		size_t amount = fread(buffer, 1, chunk_size, snapshot);
		if (amount == 0)
			break;
		hash_bytes(buffer, amount, chunk_hash);
		if (snprintf(object_rel, sizeof(object_rel), "%s/%.2s/%s",
			     DB_OBJECT_DIR, chunk_hash, chunk_hash) >=
		    (int)sizeof(object_rel)) {
			rc = -ENAMETOOLONG;
			goto out;
		}
		rc = store_stat(cfg, object_rel, &remote_stat);
		if (rc != 0)
			goto out;
		if (!remote_stat.exists) {
			rc = file_write_all(chunk_path, (const char *)buffer, amount);
			if (rc != 0)
				goto out;
			rc = store_put(cfg, chunk_path, object_rel);
			if (rc != 0)
				goto out;
			status->db_chunks_uploaded++;
			status->db_bytes_uploaded += (int64_t)amount;
		} else {
			status->db_chunks_reused++;
		}
		item = cJSON_CreateObject();
		if (!item || !cJSON_AddStringToObject(item, "hash", chunk_hash) ||
		    !cJSON_AddNumberToObject(item, "size", (double)amount)) {
			cJSON_Delete(item);
			rc = -ENOMEM;
			goto out;
		}
		cJSON_AddItemToArray(cJSON_GetObjectItem(root, "chunks"), item);
	}
	if (ferror(snapshot)) {
		rc = -EIO;
		goto out;
	}
	json = cJSON_PrintUnformatted(root);
	if (!json) {
		rc = -ENOMEM;
		goto out;
	}
	rc = file_write_all(chunk_path, json, strlen(json));
	if (rc == 0)
		rc = store_put(cfg, chunk_path, manifest_rel);
	if (rc == 0 && update_cursor)
		rc = db_cursor_put(cfg, database_key, device, database_hash);
	if (rc == 0)
		status->db_snapshots++;

out:
	if (snapshot)
		fclose(snapshot);
	free(buffer);
	free(json);
	cJSON_Delete(chunks);
	cJSON_Delete(root);
	(void)unlink(snapshot_path);
	if (chunk_path[0])
		(void)unlink(chunk_path);
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

	rc = db_backup_create_one(cfg, rel, path, status, 1);
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
		MORPH_RETURN(-ENAMETOOLONG);
	rc = file_list_files(directory, &names, &count);
	if (rc == -ENOENT)
		return 0;
	for (int i = 0; rc == 0 && i < count; i++) {
		if (strncmp(names[i], prefix, strlen(prefix)) != 0)
			continue;
		rc = file_path_join(conflict_path, sizeof(conflict_path),
				    directory, names[i]);
		if (rc == 0 && sync_db_is_sqlite(conflict_path))
			rc = db_backup_create_one(cfg, rel, conflict_path, status, 0);
	}
	file_free_list(names, count);
	return rc;
}

static int snapshot_id_valid(const char *snapshot_id)
{
	if (!snapshot_id || !snapshot_id[0])
		return 0;
	for (size_t i = 0; snapshot_id[i]; i++) {
		if (!isalnum((unsigned char)snapshot_id[i]) &&
		    snapshot_id[i] != '-' && snapshot_id[i] != '_')
			return 0;
	}
	return 1;
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
		MORPH_RETURN(-ENAMETOOLONG);
	if (cfg->remote_backend) {
		rc = staging_path(cfg, ".json", local);
		if (rc != 0)
			return rc;
		rc = store_get(cfg, rel, local);
		if (rc != 0)
			return rc;
		text = file_read_all(local, NULL);
		(void)unlink(local);
	} else {
		rc = store_path(cfg, rel, direct);
		if (rc != 0)
			return rc;
		text = file_read_all(direct, NULL);
	}
	if (!text)
		MORPH_RETURN(-ENOENT);
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
		MORPH_RETURN(MORPH_ERR_PARSE);
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
	backup = morph_array_push(scan->items);
	if (!backup) {
		cJSON_Delete(root);
		MORPH_RETURN(-ENOMEM);
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

	if (!backend_valid(cfg) || !out || !count)
		MORPH_RETURN(-EINVAL);
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
			rc = file_list_files(directory, &names, &name_count);
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
		MORPH_RETURN(MORPH_ERR_PARSE);
	if (snprintf(object_rel, sizeof(object_rel), "%s/%.2s/%s",
		     DB_OBJECT_DIR, hash->valuestring, hash->valuestring) >=
	    (int)sizeof(object_rel))
		MORPH_RETURN(-ENAMETOOLONG);
	rc = store_get(cfg, object_rel, temporary_chunk);
	if (rc != 0)
		return rc;
	data = file_read_all(temporary_chunk, &data_size);
	(void)unlink(temporary_chunk);
	if (!data)
		MORPH_RETURN(-EIO);
	hash_bytes(data, data_size, actual_hash);
	if (data_size != (size_t)size->valuedouble ||
	    strcmp(actual_hash, hash->valuestring) != 0) {
		log_err("database backup chunk failed verification: %s",
			hash->valuestring);
		free(data);
		MORPH_RETURN(MORPH_ERR_FORMAT);
	}
	if (fwrite(data, 1, data_size, output) != data_size) {
		free(data);
		MORPH_RETURN(-EIO);
	}
	free(data);
	return 0;
}

int morph_sync_restore_db(const struct morph_sync_config *cfg,
			  const char *snapshot_id, const char *destination)
{
	char manifest_name[MORPH_SYNC_SNAPSHOT_ID_MAX + 8];
	char temporary[PATH_MAX] = { 0 };
	char temporary_chunk[PATH_MAX] = { 0 };
	char actual_hash[MORPH_SYNC_HASH_LEN];
	cJSON *root = NULL;
	cJSON *chunks;
	cJSON *expected_hash;
	cJSON *expected_size;
	struct stat st;
	FILE *output = NULL;
	int rc;

	if (!backend_valid(cfg) || !snapshot_id_valid(snapshot_id) || !destination ||
	    !destination[0])
		MORPH_RETURN(-EINVAL);
	if (file_exists(destination))
		MORPH_RETURN(-EEXIST);
	if (snprintf(manifest_name, sizeof(manifest_name), "%s.json",
		     snapshot_id) >= (int)sizeof(manifest_name))
		MORPH_RETURN(-ENAMETOOLONG);
	rc = manifest_load(cfg, manifest_name, &root);
	if (rc != 0)
		return rc;
	chunks = cJSON_GetObjectItem(root, "chunks");
	expected_hash = cJSON_GetObjectItem(root, "database_hash");
	expected_size = cJSON_GetObjectItem(root, "database_size");
	if (!cJSON_IsArray(chunks) || !cJSON_IsString(expected_hash) ||
	    !cJSON_IsNumber(expected_size)) {
		rc = MORPH_ERR_PARSE;
		goto out;
	}
	rc = ensure_parent(destination);
	if (rc != 0)
		goto out;
	if (snprintf(temporary, sizeof(temporary), "%s.restore.tmp.%u",
		     destination, (unsigned)getpid()) >= (int)sizeof(temporary)) {
		rc = -ENAMETOOLONG;
		goto out;
	}
	rc = staging_path(cfg, ".chunk", temporary_chunk);
	if (rc != 0)
		goto out;
	output = fopen(temporary, "wb");
	if (!output) {
		rc = -errno;
		goto out;
	}
	cJSON *item;
	cJSON_ArrayForEach(item, chunks) {
		rc = restore_chunk(cfg, item, output, temporary_chunk);
		if (rc != 0)
			goto out;
	}
	if (fflush(output) != 0 || fclose(output) != 0) {
		output = NULL;
		rc = -EIO;
		goto out;
	}
	output = NULL;
	if (stat(temporary, &st) != 0 ||
	    (int64_t)st.st_size != (int64_t)expected_size->valuedouble) {
		log_err("database restore size mismatch for %s", snapshot_id);
		rc = MORPH_ERR_FORMAT;
		goto out;
	}
	rc = hash_file(temporary, actual_hash);
	if (rc != 0)
		goto out;
	if (strcmp(actual_hash, expected_hash->valuestring) != 0 ||
	    !quick_check(temporary)) {
		log_err("database restore integrity check failed for %s",
			snapshot_id);
		rc = MORPH_ERR_FORMAT;
		goto out;
	}
	if (rename(temporary, destination) != 0) {
		rc = -errno;
		goto out;
	}
	rc = 0;

out:
	if (output)
		fclose(output);
	if (temporary_chunk[0])
		(void)unlink(temporary_chunk);
	if (rc != 0 && temporary[0])
		(void)unlink(temporary);
	cJSON_Delete(root);
	return rc;
}

static int store_delete(const struct morph_sync_config *cfg, const char *rel)
{
	char path[PATH_MAX];

	if (cfg->remote_backend)
		return cfg->remote_backend->delete_file(
			cfg->remote_backend->user_data, rel);
	if (store_path(cfg, rel, path) != 0)
		MORPH_RETURN(-ENAMETOOLONG);
	if (unlink(path) != 0 && errno != ENOENT)
		MORPH_RETURN_ERRNO();
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
	stored = morph_array_push(scan->snapshots);
	if (!stored) {
		cJSON_Delete(root);
		MORPH_RETURN(-ENOMEM);
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
	rc = file_list_files(directory, &names, &count);
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
		MORPH_RETURN(MORPH_ERR_PARSE);
	}
	cJSON_ArrayForEach(item, chunks) {
		cJSON *hash = cJSON_GetObjectItem(item, "hash");
		if (!cJSON_IsString(hash)) {
			rc = MORPH_ERR_PARSE;
			break;
		}
		rc = morph_strmap_set(live, hash->valuestring,
				      (void *)(uintptr_t)1);
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
		MORPH_RETURN(-ENAMETOOLONG);
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
		MORPH_RETURN(-ENAMETOOLONG);
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
	rc = file_list_dirs(root, &prefixes, &prefix_count);
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
			rc = file_list_files(directory, &names, &count);
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

	if (!backend_valid(cfg))
		MORPH_RETURN(-EINVAL);
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
	if (!token || strncmp(token, "restore_", 8) != 0)
		return 0;
	for (const char *p = token; *p; p++) {
		if (!isalnum((unsigned char)*p) && *p != '_')
			return 0;
	}
	return 1;
}

static int restore_plan_valid(const char *source_dir,
			      const struct morph_sync_restore_plan *plan)
{
	char expected[PATH_MAX];
	char directory[PATH_MAX];

	if (!source_dir || !plan || !restore_token_valid(plan->token) ||
	    !restore_rel_valid(plan->path) ||
	    strlen(plan->expected_hash) != MORPH_SYNC_HASH_LEN - 1)
		return 0;
	if (file_path_join(expected, sizeof(expected), source_dir,
		plan->path) != 0 || strcmp(expected, plan->target) != 0)
		return 0;
	if (snprintf(expected, sizeof(expected), "%s.%s.tmp", plan->target,
		     plan->token) >= (int)sizeof(expected) ||
	    strcmp(expected, plan->staging) != 0)
		return 0;
	if (snprintf(expected, sizeof(expected), "%s.%s.rollback",
		     plan->target, plan->token) >= (int)sizeof(expected) ||
	    strcmp(expected, plan->rollback) != 0)
		return 0;
	if (file_path_join(directory, sizeof(directory), source_dir,
		".morph-sync/restore") != 0 ||
	    snprintf(expected, sizeof(expected), "%s/%s.json", directory,
		     plan->token) >= (int)sizeof(expected) ||
	    strcmp(expected, plan->journal) != 0)
		return 0;
	return 1;
}

static int restore_plan_source(const struct morph_sync_restore_plan *plan,
			       char source[PATH_MAX])
{
	const char marker[] = "/.morph-sync/restore/";
	const char *position;
	size_t length;

	if (!plan)
		MORPH_RETURN(-EINVAL);
	position = strstr(plan->journal, marker);
	if (!position)
		MORPH_RETURN(MORPH_ERR_FORMAT);
	length = (size_t)(position - plan->journal);
	if (length == 0 || length >= PATH_MAX)
		MORPH_RETURN(MORPH_ERR_FORMAT);
	memcpy(source, plan->journal, length);
	source[length] = '\0';
	if (!restore_plan_valid(source, plan))
		MORPH_RETURN(MORPH_ERR_FORMAT);
	return 0;
}

static int restore_sync_parent(const char *path);

static int restore_atomic_write(const char *path, const char *data,
				size_t length)
{
	char temporary[PATH_MAX];
	int fd;
	size_t offset = 0;
	int rc = 0;

	if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >=
	    (int)sizeof(temporary))
		MORPH_RETURN(-ENAMETOOLONG);
	fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		MORPH_RETURN_ERRNO();
	while (offset < length) {
		ssize_t written = write(fd, data + offset, length - offset);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0) {
			rc = written < 0 ? -errno : -EIO;
			break;
		}
		offset += (size_t)written;
	}
	if (rc == 0 && fsync(fd) != 0)
		rc = -errno;
	if (close(fd) != 0 && rc == 0)
		rc = -errno;
	if (rc == 0 && rename(temporary, path) != 0)
		rc = -errno;
	if (rc == 0)
		rc = restore_sync_parent(path);
	if (rc != 0)
		(void)unlink(temporary);
	return rc;
}

static int restore_sync_parent(const char *path)
{
	char parent[PATH_MAX];
	char *slash;
	int fd;
	int rc = 0;

	if (!path || strlen(path) >= sizeof(parent))
		MORPH_RETURN(-EINVAL);
	strncpy(parent, path, sizeof(parent) - 1);
	parent[sizeof(parent) - 1] = '\0';
	slash = strrchr(parent, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	fd = open(parent, O_RDONLY);
	if (fd < 0)
		MORPH_RETURN_ERRNO();
	if (fsync(fd) != 0 && errno != EINVAL && errno != EROFS)
		rc = -errno;
	if (close(fd) != 0 && rc == 0)
		rc = -errno;
	return rc;
}

static int restore_unlink_optional(const char *path)
{
	if (unlink(path) != 0 && errno != ENOENT)
		MORPH_RETURN_ERRNO();
	return 0;
}

static int restore_journal_write(const struct morph_sync_restore_plan *plan,
				 const char *phase)
{
	cJSON *root;
	char *json;
	int rc = 0;

	root = cJSON_CreateObject();
	if (!root)
		MORPH_RETURN(-ENOMEM);
	if (!cJSON_AddStringToObject(root, "phase", phase) ||
	    !cJSON_AddStringToObject(root, "token", plan->token) ||
	    !cJSON_AddStringToObject(root, "snapshot_id", plan->snapshot_id) ||
	    !cJSON_AddStringToObject(root, "path", plan->path) ||
	    !cJSON_AddStringToObject(root, "target", plan->target) ||
	    !cJSON_AddStringToObject(root, "staging", plan->staging) ||
	    !cJSON_AddStringToObject(root, "rollback", plan->rollback) ||
	    !cJSON_AddStringToObject(root, "journal", plan->journal) ||
	    !cJSON_AddStringToObject(root, "expected_hash",
				     plan->expected_hash)) {
		cJSON_Delete(root);
		MORPH_RETURN(-ENOMEM);
	}
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json)
		MORPH_RETURN(-ENOMEM);
	rc = restore_atomic_write(plan->journal, json, strlen(json));
	free(json);
	return rc;
}

static int restore_plan_from_json(const char *journal,
				  struct morph_sync_restore_plan *plan,
				  char phase[16])
{
	char *text;
	cJSON *root;
	const char *names[] = { "token", "snapshot_id", "path", "target",
		"staging", "rollback", "journal", "expected_hash" };
	char *values[] = { plan->token, plan->snapshot_id, plan->path,
		plan->target, plan->staging, plan->rollback, plan->journal,
		plan->expected_hash };
	size_t sizes[] = { sizeof(plan->token), sizeof(plan->snapshot_id),
		sizeof(plan->path), sizeof(plan->target), sizeof(plan->staging),
		sizeof(plan->rollback), sizeof(plan->journal),
		sizeof(plan->expected_hash) };
	cJSON *item;

	text = file_read_all(journal, NULL);
	if (!text)
		MORPH_RETURN(-EIO);
	root = cJSON_Parse(text);
	free(text);
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		MORPH_RETURN(MORPH_ERR_PARSE);
	}
	memset(plan, 0, sizeof(*plan));
	item = cJSON_GetObjectItem(root, "phase");
	if (!cJSON_IsString(item)) {
		cJSON_Delete(root);
		MORPH_RETURN(MORPH_ERR_PARSE);
	}
	strncpy(phase, item->valuestring, 15);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		item = cJSON_GetObjectItem(root, names[i]);
		if (!cJSON_IsString(item) ||
		    strlen(item->valuestring) >= sizes[i]) {
			cJSON_Delete(root);
			MORPH_RETURN(MORPH_ERR_PARSE);
		}
		strncpy(values[i], item->valuestring, sizes[i] - 1);
	}
	cJSON_Delete(root);
	return 0;
}

static void restore_sidecar_path(const char *path, const char *suffix,
				 char out[PATH_MAX])
{
	(void)snprintf(out, PATH_MAX, "%s%s", path, suffix);
}

int morph_sync_prepare_db_replace(const struct morph_sync_config *cfg,
				  const char *snapshot_id,
				  struct morph_sync_restore_plan *plan)
{
	struct morph_sync_backup *items = NULL;
	struct morph_sync_status status;
	char journal_dir[PATH_MAX];
	int count = 0;
	int found = -1;
	int rc;

	if (!backend_valid(cfg) || !snapshot_id || !plan)
		MORPH_RETURN(-EINVAL);
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
		MORPH_RETURN(found < 0 ? -ENOENT : MORPH_ERR_FORMAT);
	}
	strncpy(plan->snapshot_id, snapshot_id,
		sizeof(plan->snapshot_id) - 1);
	strncpy(plan->path, items[found].path, sizeof(plan->path) - 1);
	strncpy(plan->expected_hash, items[found].hash,
		sizeof(plan->expected_hash) - 1);
	morph_sync_backups_free(items);
	rc = file_path_join(plan->target, sizeof(plan->target),
		cfg->source_dir, plan->path);
	if (rc != 0)
		return rc;
	rc = morph_random_id("restore_", plan->token, sizeof(plan->token));
	if (rc != 0)
		return rc;
	if (snprintf(plan->staging, sizeof(plan->staging), "%s.%s.tmp",
		     plan->target, plan->token) >= (int)sizeof(plan->staging) ||
	    snprintf(plan->rollback, sizeof(plan->rollback), "%s.%s.rollback",
		     plan->target, plan->token) >= (int)sizeof(plan->rollback))
		MORPH_RETURN(-ENAMETOOLONG);
	rc = file_path_join(journal_dir, sizeof(journal_dir), cfg->source_dir,
		".morph-sync/restore");
	if (rc == 0)
		rc = file_ensure_dir(journal_dir);
	if (rc != 0)
		return rc;
	if (snprintf(plan->journal, sizeof(plan->journal), "%s/%s.json",
		     journal_dir, plan->token) >= (int)sizeof(plan->journal))
		MORPH_RETURN(-ENAMETOOLONG);
	if (!restore_plan_valid(cfg->source_dir, plan))
		MORPH_RETURN(MORPH_ERR_FORMAT);
	if (sync_db_is_sqlite(plan->target)) {
		memset(&status, 0, sizeof(status));
		rc = db_backup_create_one(cfg, plan->path, plan->target,
			&status, 1);
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

static int restore_move_sidecar(const char *from, const char *to,
				const char *suffix)
{
	char source[PATH_MAX];
	char destination[PATH_MAX];

	restore_sidecar_path(from, suffix, source);
	restore_sidecar_path(to, suffix, destination);
	if (file_exists(source) && rename(source, destination) != 0)
		MORPH_RETURN_ERRNO();
	return 0;
}

int morph_sync_apply_db_replace(struct morph_sync_restore_plan *plan)
{
	int moved = 0;
	char actual_hash[MORPH_SYNC_HASH_LEN];
	char source[PATH_MAX];
	int rc;

	if (!plan)
		MORPH_RETURN(-EINVAL);
	rc = restore_plan_source(plan, source);
	if (rc != 0)
		return rc;
	if (!quick_check(plan->staging))
		MORPH_RETURN(MORPH_ERR_FORMAT);
	rc = hash_file(plan->staging, actual_hash);
	if (rc != 0)
		return rc;
	if (strcmp(actual_hash, plan->expected_hash) != 0)
		MORPH_RETURN(MORPH_ERR_FORMAT);
	rc = restore_journal_write(plan, "applying");
	if (rc != 0)
		return rc;
	if (file_exists(plan->target)) {
		if (rename(plan->target, plan->rollback) != 0)
			MORPH_RETURN_ERRNO();
		moved = 1;
		rc = restore_move_sidecar(plan->target, plan->rollback, "-wal");
		if (rc == 0)
			rc = restore_move_sidecar(plan->target, plan->rollback,
				"-shm");
		if (rc != 0) {
			(void)rename(plan->rollback, plan->target);
			(void)restore_move_sidecar(plan->rollback, plan->target,
				"-wal");
			(void)restore_move_sidecar(plan->rollback, plan->target,
				"-shm");
			return rc;
		}
	}
	if (rename(plan->staging, plan->target) != 0) {
		rc = -errno;
		if (moved) {
			(void)rename(plan->rollback, plan->target);
			(void)restore_move_sidecar(plan->rollback, plan->target,
				"-wal");
			(void)restore_move_sidecar(plan->rollback, plan->target,
				"-shm");
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
		MORPH_RETURN(MORPH_ERR_FORMAT);
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
		MORPH_RETURN(MORPH_ERR_FORMAT);
	if (file_exists(plan->rollback)) {
		rc = restore_unlink_optional(plan->target);
		if (rc != 0)
			return rc;
		if (rename(plan->rollback, plan->target) != 0)
			MORPH_RETURN_ERRNO();
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

static int restore_journals_process(const char *source_dir, int finalize)
{
	char directory[PATH_MAX];
	char journal[PATH_MAX];
	char **names = NULL;
	int count = 0;
	int rc;

	rc = file_path_join(directory, sizeof(directory), source_dir,
		".morph-sync/restore");
	if (rc != 0)
		return rc;
	rc = file_list_files(directory, &names, &count);
	if (rc == -ENOENT)
		return 0;
	for (int i = 0; rc == 0 && i < count; i++) {
		struct morph_sync_restore_plan plan;
		char phase[16] = { 0 };

		rc = file_path_join(journal, sizeof(journal), directory, names[i]);
		if (rc == 0)
			rc = restore_plan_from_json(journal, &plan, phase);
		if (rc == 0 && (!restore_plan_valid(source_dir, &plan) ||
				     strcmp(journal, plan.journal) != 0))
			rc = MORPH_ERR_FORMAT;
		if (rc != 0) {
			log_warn("ignoring invalid restore journal %s: %s", journal,
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

int morph_sync_recover_db_replacements(const char *source_dir)
{
	if (!source_dir)
		MORPH_RETURN(-EINVAL);
	return restore_journals_process(source_dir, 0);
}

int morph_sync_finalize_db_replacements(const char *source_dir)
{
	if (!source_dir)
		MORPH_RETURN(-EINVAL);
	return restore_journals_process(source_dir, 1);
}

char *morph_sync_restore_plan_to_json(
	const struct morph_sync_restore_plan *plan)
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

int morph_sync_restore_plan_from_json(const char *json,
	struct morph_sync_restore_plan *plan)
{
	cJSON *root;
	const char *names[] = { "token", "snapshot_id", "path", "target",
		"staging", "rollback", "journal", "expected_hash" };
	char *values[] = { plan ? plan->token : NULL,
		plan ? plan->snapshot_id : NULL, plan ? plan->path : NULL,
		plan ? plan->target : NULL, plan ? plan->staging : NULL,
		plan ? plan->rollback : NULL, plan ? plan->journal : NULL,
		plan ? plan->expected_hash : NULL };
	size_t sizes[] = { plan ? sizeof(plan->token) : 0,
		plan ? sizeof(plan->snapshot_id) : 0,
		plan ? sizeof(plan->path) : 0, plan ? sizeof(plan->target) : 0,
		plan ? sizeof(plan->staging) : 0,
		plan ? sizeof(plan->rollback) : 0,
		plan ? sizeof(plan->journal) : 0,
		plan ? sizeof(plan->expected_hash) : 0 };

	if (!json || !plan)
		MORPH_RETURN(-EINVAL);
	memset(plan, 0, sizeof(*plan));
	root = cJSON_Parse(json);
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		MORPH_RETURN(MORPH_ERR_PARSE);
	}
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		cJSON *item = cJSON_GetObjectItem(root, names[i]);

		if (!cJSON_IsString(item) ||
		    strlen(item->valuestring) >= sizes[i]) {
			cJSON_Delete(root);
			MORPH_RETURN(MORPH_ERR_PARSE);
		}
		strncpy(values[i], item->valuestring, sizes[i] - 1);
	}
	cJSON_Delete(root);
	return 0;
}
