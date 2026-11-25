#include "sync/db_backup.h"
#include <limits.h>
#include <nb/vec.h>
#include <nb/cleanup.h>
#include "errors.h"
#include "cJSON.h"
#include <nb/file.h>
#include <nb/random.h>
#include <nb/encoding.h>
#include <nb/log.h>
#include <nb/map.h>
#include <nb/str.h>
#include "content_hash.h"
#include "persistence/orm/file_database.h"
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
	nb_vec *items;
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

	(void)nb_str_copy_to(nb_str_from_parts((path), strnlen((path), sizeof(parent) - 1)), parent,
			     sizeof(parent));
	parent[sizeof(parent) - 1] = '\0';
	slash = strrchr(parent, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	return parent[0] ? nb_file_mkdir(parent, true, NULL) : 0;
}

static int store_path(const struct morph_sync_config *cfg, const char *rel,
		      char out[PATH_MAX])
{
	nb_buf store_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int store_path_status = NB_OK;

	return ((store_path_status = nb_path_concat(&store_path_buffer, cfg->sync_dir, rel)) !=
				NB_OK
			? store_path_status
		: (store_path_status = nb_str_copy_to(nb_buf_str(&store_path_buffer), out,
						      PATH_MAX)) == NB_ERR_RANGE
			? NB_ERR_NAME_TOO_LONG
			: store_path_status);
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
		return NB_ERR_NAME_TOO_LONG;
	if (stat(path, &st) != 0)
		return errno == ENOENT ? 0 : nb_status_from_errno(errno, NULL);
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

	if (cfg->remote_backend)
		return cfg->remote_backend->copy_from_local(
			cfg->remote_backend->user_data, source, rel, 0);
	int rc = store_path(cfg, rel, destination);
	if (rc == 0)
		rc = ensure_parent(destination);
	if (rc != 0)
		return rc;
	return nb_file_copy(source, destination, &(nb_file_copy_options){.overwrite = true}, NULL);
}

static int store_get(const struct morph_sync_config *cfg, const char *rel,
		     const char *destination)
{
	char source[PATH_MAX];

	if (cfg->remote_backend)
		return cfg->remote_backend->copy_to_local(
			cfg->remote_backend->user_data, rel, destination);
	if (store_path(cfg, rel, source) != 0)
		return NB_ERR_NAME_TOO_LONG;
	int rc = ensure_parent(destination);
	if (rc != NB_OK)
		return rc;
	return nb_file_copy(source, destination, &(nb_file_copy_options){.overwrite = true}, NULL);
}

int sync_db_quick_check(const char *path)
{
	return orm_database_quick_check(path);
}

int sync_db_is_sqlite(const char *path)
{
	return orm_database_is_sqlite(path);
}

static int device_id_get(const struct morph_sync_config *cfg,
			 char out[MORPH_SYNC_DEVICE_ID_MAX])
{
	nb_buf metadata_dir_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int metadata_dir_status = NB_OK;
	nb_buf device_id_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int device_id_path_status = NB_OK;
	nb_buf device_id_contents_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	char *device_id_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;
	nb_buf device_id_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int device_id_status = NB_OK;

	char directory[PATH_MAX];
	char path[PATH_MAX];
	char *stored;
	int rc;

	rc = ((metadata_dir_status = nb_path_concat(&metadata_dir_buffer, cfg->source_dir,
						    ".morph-sync")) != NB_OK
		      ? metadata_dir_status
	      : (metadata_dir_status = nb_str_copy_to(nb_buf_str(&metadata_dir_buffer), directory,
						      sizeof(directory))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : metadata_dir_status);
	if (rc != 0)
		return rc;
	rc = nb_file_mkdir(directory, true, NULL);
	if (rc != 0)
		return rc;
	rc = ((device_id_path_status =
		       nb_path_concat(&device_id_path_buffer, directory, "device-id")) != NB_OK
		      ? device_id_path_status
	      : (device_id_path_status = nb_str_copy_to(nb_buf_str(&device_id_path_buffer), path,
							sizeof(path))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : device_id_path_status);
	if (rc != 0)
		return rc;
	stored =
		(nb_file_read_all(path, &device_id_contents_buffer, 0, NULL) == NB_OK &&
				 (device_id_contents_copy = (char *)malloc(
					  nb_buf_len(&device_id_contents_buffer) + 1)) != NULL
			 ? (memcpy(device_id_contents_copy, nb_buf_cstr(&device_id_contents_buffer),
				   nb_buf_len(&device_id_contents_buffer) + 1),
			    nb_charp_steal(&device_id_contents_copy))
			 : NULL);
	if (stored) {
		stored[strcspn(stored, "\r\n")] = '\0';
		if (stored[0]) {
			(void)nb_str_copy_to(
				nb_str_from_parts((stored),
						  strnlen((stored), MORPH_SYNC_DEVICE_ID_MAX - 1)),
				out, MORPH_SYNC_DEVICE_ID_MAX);
			free(stored);
			return 0;
		}
		free(stored);
	}
	rc = ((device_id_status = ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
				   : !(nb_buf_clear(&device_id_buffer),
				       nb_buf_append_cstr(&device_id_buffer, "dev_"))
					   ? NB_ERR_NOMEM
					   : nb_random_hex(&device_id_buffer, 16, NULL))) != NB_OK
		      ? device_id_status
	      : (device_id_status = nb_str_copy_to(nb_buf_str(&device_id_buffer), out,
						   MORPH_SYNC_DEVICE_ID_MAX)) == NB_ERR_RANGE
		      ? NB_ERR_NO_SPACE
		      : device_id_status);
	if (rc != 0)
		return rc;
	return nb_file_write_all(path, out, strlen(out), NULL);
}

static int db_cursor_path(const struct morph_sync_config *cfg,
			  const char *database_key, char out[PATH_MAX])
{
	nb_buf database_dir_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int database_dir_status = NB_OK;
	nb_buf database_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int database_path_status = NB_OK;

	char directory[PATH_MAX];
	int rc;

	rc = ((database_dir_status = nb_path_concat(&database_dir_buffer, cfg->source_dir,
						    ".morph-sync/db-state")) != NB_OK
		      ? database_dir_status
	      : (database_dir_status = nb_str_copy_to(nb_buf_str(&database_dir_buffer), directory,
						      sizeof(directory))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : database_dir_status);
	if (rc != 0)
		return rc;
	rc = nb_file_mkdir(directory, true, NULL);
	if (rc != 0)
		return rc;
	return ((database_path_status =
			 nb_path_concat(&database_path_buffer, directory, database_key)) != NB_OK
			? database_path_status
		: (database_path_status = nb_str_copy_to(nb_buf_str(&database_path_buffer), out,
							 PATH_MAX)) == NB_ERR_RANGE
			? NB_ERR_NAME_TOO_LONG
			: database_path_status);
}

static int db_cursor_matches(const struct morph_sync_config *cfg,
			     const char *database_key, const char *device,
			     const char *database_hash)
{
	nb_buf stored_contents_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	char *stored_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;

	char path[PATH_MAX];
	char expected[MORPH_SYNC_DEVICE_ID_MAX + MORPH_SYNC_HASH_LEN + 2];
	char *stored;
	int matches;

	if (db_cursor_path(cfg, database_key, path) != 0)
		return 0;
	stored = (nb_file_read_all(path, &stored_contents_buffer, 0, NULL) == NB_OK &&
				  (stored_contents_copy = (char *)malloc(
					   nb_buf_len(&stored_contents_buffer) + 1)) != NULL
			  ? (memcpy(stored_contents_copy, nb_buf_cstr(&stored_contents_buffer),
				    nb_buf_len(&stored_contents_buffer) + 1),
			     nb_charp_steal(&stored_contents_copy))
			  : NULL);
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
		return NB_ERR_NAME_TOO_LONG;
	return nb_file_write_all(path, value, strlen(value), NULL);
}

static int staging_path(const struct morph_sync_config *cfg, const char *suffix,
			char out[PATH_MAX])
{
	nb_buf temporary_dir_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int temporary_dir_status = NB_OK;
	nb_buf random_id_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int random_id_status = NB_OK;

	char directory[PATH_MAX];
	char random_id[64];
	int rc;

	out[0] = '\0';
	rc = ((temporary_dir_status = nb_path_concat(&temporary_dir_buffer, cfg->sync_dir,
						     ".morph-sync/staging")) != NB_OK
		      ? temporary_dir_status
	      : (temporary_dir_status = nb_str_copy_to(nb_buf_str(&temporary_dir_buffer), directory,
						       sizeof(directory))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : temporary_dir_status);
	if (rc != 0)
		return rc;
	rc = nb_file_mkdir(directory, true, NULL);
	if (rc != 0)
		return rc;
	rc = ((random_id_status = ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
				   : !(nb_buf_clear(&random_id_buffer),
				       nb_buf_append_cstr(&random_id_buffer, "db_"))
					   ? NB_ERR_NOMEM
					   : nb_random_hex(&random_id_buffer, 16, NULL))) != NB_OK
		      ? random_id_status
	      : (random_id_status = nb_str_copy_to(nb_buf_str(&random_id_buffer), random_id,
						   sizeof(random_id))) == NB_ERR_RANGE
		      ? NB_ERR_NO_SPACE
		      : random_id_status);
	if (rc != 0)
		return rc;
	int length = snprintf(out, PATH_MAX, "%s/%s%s", directory, random_id,
			      suffix ? suffix : "");
	if (length < 0 || length >= PATH_MAX) {
		out[0] = '\0';
		return NB_ERR_NAME_TOO_LONG;
	}
	return 0;
}

static int json_add_string(cJSON *object, const char *name, const char *value)
{
	return cJSON_AddStringToObject(object, name, value) ? 0 : NB_ERR_NOMEM;
}

/* Bundle display history into the core snapshot, never into the live database.
 * The core transcript remains authoritative if a UI writer was ahead/behind.
 * Readers validate message anchors before accepting this optional projection. */
static int snapshot_include_display(const struct morph_sync_config *cfg,
				    const char *rel, const char *snapshot)
{
	nb_buf source_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int source_path_status = NB_OK;

	char source[PATH_MAX];
	struct backup_staging staging NB_CLEANUP(backup_staging_cleanup) = {0};
	struct stat st;
	int page_size = 0;
	int rc;

	if (strcmp(rel, "data.db") != 0)
		return 0;
	rc = ((source_path_status = nb_path_concat(&source_path_buffer, cfg->source_dir,
						   "ui-history.db")) != NB_OK
		      ? source_path_status
	      : (source_path_status = nb_str_copy_to(nb_buf_str(&source_path_buffer), source,
						     sizeof(source))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : source_path_status);
	if (rc != 0)
		return rc;
	if (stat(source, &st) != 0) {
		if (errno == ENOENT)
			return 0;
		return nb_status_from_errno(errno, NULL);
	}
	rc = staging_path(cfg, ".display.db", staging.snapshot);
	if (rc != 0)
		return rc;
	rc = orm_database_snapshot(source, staging.snapshot, &page_size);
	if (rc != 0)
		return rc;
	return orm_database_merge_display(snapshot, staging.snapshot);
}

int sync_db_backup_create_one(const struct morph_sync_config *cfg, const char *rel,
			      const char *path, struct morph_sync_status *status, int update_cursor)
{
	nb_buf version_id_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int version_id_status = NB_OK;

	struct backup_staging staging NB_CLEANUP(backup_staging_cleanup) = {0};
	char object_rel[PATH_MAX];
	char manifest_rel[PATH_MAX];
	char database_hash[MORPH_SYNC_HASH_LEN];
	char database_key[MORPH_SYNC_HASH_LEN];
	char device[MORPH_SYNC_DEVICE_ID_MAX] = { 0 };
	char snapshot_id[MORPH_SYNC_SNAPSHOT_ID_MAX];
	char version_id[MORPH_SYNC_HASH_LEN];
	struct morph_sync_backend_stat remote_stat;
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	cJSON *chunks NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	unsigned char *buffer NB_CLEANUP(backup_bytes_cleanup) = NULL;
	char *json NB_CLEANUP(cJSON_FreeTextOwner) = NULL;
	FILE *snapshot NB_CLEANUP(backup_file_cleanup) = NULL;
	int page_size = 0;
	size_t chunk_size;
	int rc;

	if (!sync_db_backend_valid(cfg) || !rel || !path || !status)
		return NB_ERR_INVAL;
	rc = staging_path(cfg, ".db", staging.snapshot);
	if (rc != 0)
		return rc;
	rc = orm_database_snapshot(path, staging.snapshot, &page_size);
	if (rc != 0)
		return rc;
	if (update_cursor) {
		rc = snapshot_include_display(cfg, rel, staging.snapshot);
		if (rc != 0)
			return rc;
	}
	if (!sync_db_quick_check(staging.snapshot)) {
		return NB_ERR_FORMAT;
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
		rc = ((version_id_status =
			       ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
				: !(nb_buf_clear(&version_id_buffer),
				    nb_buf_append_cstr(&version_id_buffer, "ver_"))
					? NB_ERR_NOMEM
					: nb_random_hex(&version_id_buffer, 16, NULL))) != NB_OK
			      ? version_id_status
		      : (version_id_status = nb_str_copy_to(nb_buf_str(&version_id_buffer),
							    version_id, sizeof(version_id))) ==
				      NB_ERR_RANGE
			      ? NB_ERR_NO_SPACE
			      : version_id_status);
		if (rc != 0)
			return rc;
	} else {
		(void)nb_str_copy_to(
			nb_str_from_parts((database_hash),
					  strnlen((database_hash), sizeof(version_id) - 1)),
			version_id, sizeof(version_id));
		version_id[sizeof(version_id) - 1] = '\0';
	}
	if (snprintf(snapshot_id, sizeof(snapshot_id), "%s-%s-%s",
		     database_key, device, version_id) >=
	    (int)sizeof(snapshot_id)) {
		return NB_ERR_NAME_TOO_LONG;
	}
	if (snprintf(manifest_rel, sizeof(manifest_rel), "%s/%s.json",
		     DB_SNAPSHOT_DIR, snapshot_id) >= (int)sizeof(manifest_rel)) {
		return NB_ERR_NAME_TOO_LONG;
	}
	rc = store_stat(cfg, manifest_rel, &remote_stat);
	if (rc != 0 || remote_stat.exists)
		return rc;

	root = cJSON_CreateObject();
	chunks = cJSON_CreateArray();
	if (!root || !chunks) {
		return NB_ERR_NOMEM;
	}
	cJSON_AddNumberToObject(root, "version", 1);
	if (json_add_string(root, "snapshot_id", snapshot_id) != 0 ||
	    json_add_string(root, "path", rel) != 0 ||
	    json_add_string(root, "device_id", device) != 0 ||
	    json_add_string(root, "database_hash", database_hash) != 0) {
		return NB_ERR_NOMEM;
	}
	cJSON_AddNumberToObject(root, "created_at", (double)time(NULL));
	{
		struct stat st;
		if (stat(staging.snapshot, &st) != 0) {
			return nb_status_from_errno(errno, NULL);
		}
		cJSON_AddNumberToObject(root, "database_size", (double)st.st_size);
	}
	cJSON_AddNumberToObject(root, "page_size", page_size);
	chunk_size = DB_CHUNK_TARGET / (size_t)page_size * (size_t)page_size;
	if (chunk_size == 0)
		chunk_size = (size_t)page_size;
	cJSON_AddNumberToObject(root, "chunk_size", (double)chunk_size);
	rc = (cJSON_AddItemToObject(root, "chunks", chunks) ? (chunks = NULL, NB_OK)
							    : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	buffer = malloc(chunk_size);
	if (!buffer) {
		return NB_ERR_NOMEM;
	}
	snapshot = fopen(staging.snapshot, "rb");
	if (!snapshot) {
		return nb_status_from_errno(errno, NULL);
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
			return NB_ERR_NAME_TOO_LONG;
		}
		rc = store_stat(cfg, object_rel, &remote_stat);
		if (rc != 0)
			return rc;
		if (!remote_stat.exists) {
			rc = nb_file_write_all(staging.chunk, (const char *)buffer, amount, NULL);
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
			return NB_ERR_NOMEM;
		}
		cJSON_AddItemToArray(cJSON_GetObjectItem(root, "chunks"), item);
	}
	if (ferror(snapshot)) {
		return NB_ERR_IO;
	}
	json = cJSON_PrintUnformatted(root);
	if (!json) {
		return NB_ERR_NOMEM;
	}
	rc = nb_file_write_all(staging.chunk, json, strlen(json), NULL);
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
	int directory_scan_status = NB_OK;

	nb_buf conflict_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int conflict_path_status = NB_OK;

	char directory[PATH_MAX];
	char prefix[PATH_MAX];
	char conflict_path[PATH_MAX];
	nb_dir_listing names NB_CLEANUP(nb_dir_listing_destroy) = {0};
	char *slash;
	const char *base;
	int count = 0;
	int rc;

	rc = sync_db_backup_create_one(cfg, rel, path, status, 1);
	if (rc != 0)
		return rc;
	(void)nb_str_copy_to(nb_str_from_parts((path), strnlen((path), sizeof(directory) - 1)),
			     directory, sizeof(directory));
	directory[sizeof(directory) - 1] = '\0';
	slash = strrchr(directory, '/');
	if (slash) {
		*slash = '\0';
		base = slash + 1;
	} else {
		(void)nb_str_copy_to(NB_STR_LIT("."), directory, sizeof(directory));
		base = path;
	}
	if (snprintf(prefix, sizeof(prefix), "%s.conflict.", base) >=
	    (int)sizeof(prefix))
		return NB_ERR_NAME_TOO_LONG;
	rc = ((directory_scan_status = nb_dir_scan(
		       directory, NB_DIR_SCAN_FILES | NB_DIR_SCAN_SORT | NB_DIR_SCAN_FOLLOW_LINKS,
		       &names, NULL)) != NB_OK
		      ? directory_scan_status
	      : names.names.len > INT_MAX ? NB_ERR_RANGE
					  : (count = (int)names.names.len, NB_OK));
	if (rc == NB_ERR_NOT_FOUND)
		return 0;
	for (int i = 0; rc == 0 && i < count; i++) {
		if (strncmp(nb_dir_listing_name(&names, (size_t)(i)), prefix, strlen(prefix)) != 0)
			continue;
		rc = ((conflict_path_status =
			       nb_path_concat(&conflict_path_buffer, directory,
					      nb_dir_listing_name(&names, (size_t)(i)))) != NB_OK
			      ? conflict_path_status
		      : (conflict_path_status =
				 nb_str_copy_to(nb_buf_str(&conflict_path_buffer), conflict_path,
						sizeof(conflict_path))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : conflict_path_status);
		if (rc == 0 && sync_db_is_sqlite(conflict_path))
			rc = sync_db_backup_create_one(cfg, rel, conflict_path, status, 0);
	}
	nb_dir_listing_destroy(&names);
	return rc;
}

static int snapshot_id_valid(const char *snapshot_id)
{
	return nb_str_is_alnum(nb_str_from_cstr(snapshot_id), "-_");
}

static int manifest_load(const struct morph_sync_config *cfg, const char *name,
			 cJSON **out)
{
	nb_buf local_contents_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	char *local_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;
	nb_buf direct_contents_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	char *direct_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;

	char rel[PATH_MAX];
	char local[PATH_MAX] = { 0 };
	char direct[PATH_MAX];
	char *text;
	int rc;

	if (snprintf(rel, sizeof(rel), "%s/%s", DB_SNAPSHOT_DIR, name) >=
	    (int)sizeof(rel))
		return NB_ERR_NAME_TOO_LONG;
	if (cfg->remote_backend) {
		rc = staging_path(cfg, ".json", local);
		if (rc != 0)
			return rc;
		rc = store_get(cfg, rel, local);
		if (rc != 0)
			return rc;
		text = (nb_file_read_all(local, &local_contents_buffer, 0, NULL) == NB_OK &&
					(local_contents_copy = (char *)malloc(
						 nb_buf_len(&local_contents_buffer) + 1)) != NULL
				? (memcpy(local_contents_copy, nb_buf_cstr(&local_contents_buffer),
					  nb_buf_len(&local_contents_buffer) + 1),
				   nb_charp_steal(&local_contents_copy))
				: NULL);
		(void)unlink(local);
	} else {
		rc = store_path(cfg, rel, direct);
		if (rc != 0)
			return rc;
		text = (nb_file_read_all(direct, &direct_contents_buffer, 0, NULL) == NB_OK &&
					(direct_contents_copy = (char *)malloc(
						 nb_buf_len(&direct_contents_buffer) + 1)) != NULL
				? (memcpy(direct_contents_copy,
					  nb_buf_cstr(&direct_contents_buffer),
					  nb_buf_len(&direct_contents_buffer) + 1),
				   nb_charp_steal(&direct_contents_copy))
				: NULL);
	}
	if (!text)
		return NB_ERR_NOT_FOUND;
	*out = cJSON_Parse(text);
	free(text);
	return *out ? 0 : NB_ERR_PARSE;
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
		return NB_ERR_PARSE;
	memset(backup, 0, sizeof(*backup));
	(void)nb_str_copy_to(nb_str_from_parts((snapshot_id->valuestring),
					       strnlen((snapshot_id->valuestring),
						       sizeof(backup->snapshot_id) - 1)),
			     backup->snapshot_id, sizeof(backup->snapshot_id));
	(void)nb_str_copy_to(
		nb_str_from_parts((path->valuestring),
				  strnlen((path->valuestring), sizeof(backup->path) - 1)),
		backup->path, sizeof(backup->path));
	(void)nb_str_copy_to(
		nb_str_from_parts((device->valuestring),
				  strnlen((device->valuestring), sizeof(backup->device_id) - 1)),
		backup->device_id, sizeof(backup->device_id));
	(void)nb_str_copy_to(
		nb_str_from_parts((hash->valuestring),
				  strnlen((hash->valuestring), sizeof(backup->hash) - 1)),
		backup->hash, sizeof(backup->hash));
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
	backup = nb_vec_push(scan->items);
	if (!backup) {
		cJSON_Delete(root);
		return NB_ERR_NOMEM;
	}
	rc = backup_from_json(root, backup);
	cJSON_Delete(root);
	if (rc != 0) {
		scan->items->len--;
		return rc;
	}
	if (scan->path_filter && scan->path_filter[0] &&
	    strcmp(scan->path_filter, backup->path) != 0)
		scan->items->len--;
	return 0;
}

static int snapshot_remote_cb(const char *name, int is_dir, void *user_data)
{
	struct snapshot_scan *scan = user_data;

	if (!scan || scan->rc != 0 || is_dir)
		return scan ? scan->rc : NB_ERR_INVAL;
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
	int directory_scan_status = NB_OK;

	struct snapshot_scan scan;
	nb_vec items;
	char directory[PATH_MAX];
	nb_dir_listing names NB_CLEANUP(nb_dir_listing_destroy) = {0};
	int name_count = 0;
	int rc;

	if (!sync_db_backend_valid(cfg) || !out || !count)
		return NB_ERR_INVAL;
	*out = NULL;
	*count = 0;
	rc = (nb_vec_init(&items, sizeof(struct morph_sync_backup)),
	      nb_vec_reserve(&items, 8) ? 0 : NB_ERR_NOMEM);
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
			rc = ((directory_scan_status =
				       nb_dir_scan(directory,
						   NB_DIR_SCAN_FILES | NB_DIR_SCAN_SORT |
							   NB_DIR_SCAN_FOLLOW_LINKS,
						   &names, NULL)) != NB_OK
				      ? directory_scan_status
			      : names.names.len > INT_MAX
				      ? NB_ERR_RANGE
				      : (name_count = (int)names.names.len, NB_OK));
		if (rc == NB_ERR_NOT_FOUND)
			rc = 0;
		for (int i = 0; rc == 0 && i < name_count; i++)
			rc = snapshot_scan_name(&scan, nb_dir_listing_name(&names, (size_t)(i)));
		nb_dir_listing_destroy(&names);
	}
	if (rc != 0) {
		nb_vec_cleanup(&items);
		return rc;
	}
	if (items.len > 1)
		qsort(((void *)items.data), items.len, sizeof(struct morph_sync_backup),
		      backup_compare);
	*out = items.len ? malloc(items.len * items.elem_size) : NULL;
	if (items.len && !*out) {
		nb_vec_cleanup(&items);
		return NB_ERR_NOMEM;
	}
	if (items.len)
		memcpy(*out, items.data, items.len * items.elem_size);
	*count = (int)items.len;
	items.len = 0;
	nb_vec_cleanup(&items);
	return 0;
}

void morph_sync_backups_free(struct morph_sync_backup *items)
{
	free(items);
}

static int restore_chunk(const struct morph_sync_config *cfg, cJSON *item,
			 FILE *output, const char *temporary_chunk)
{
	nb_buf data NB_CLEANUP(nb_buf_cleanup) = {0};
	char object_rel[PATH_MAX];
	char actual_hash[MORPH_SYNC_HASH_LEN];
	cJSON *hash = cJSON_GetObjectItem(item, "hash");
	cJSON *size = cJSON_GetObjectItem(item, "size");

	if (!cJSON_IsString(hash) || strlen(hash->valuestring) != 64 ||
	    !cJSON_IsNumber(size) || size->valuedouble < 0)
		return NB_ERR_PARSE;
	if (snprintf(object_rel, sizeof(object_rel), "%s/%.2s/%s",
		     DB_OBJECT_DIR, hash->valuestring, hash->valuestring) >=
	    (int)sizeof(object_rel))
		return NB_ERR_NAME_TOO_LONG;
	int rc = store_get(cfg, object_rel, temporary_chunk);
	if (rc != 0)
		return rc;
	rc = nb_file_read_all(temporary_chunk, &data, 0, NULL);
	(void)unlink(temporary_chunk);
	if (rc != 0)
		return rc;
	sync_hash_bytes(nb_buf_cstr(&data), nb_buf_len(&data), actual_hash);
	if (nb_buf_len(&data) != (size_t)size->valuedouble ||
	    strcmp(actual_hash, hash->valuestring) != 0) {
		nb_log_write(cfg ? cfg->logger : NULL, NB_LOG_ERROR,
			     "database backup chunk failed verification: %s", hash->valuestring);
		return NB_ERR_FORMAT;
	}
	if (fwrite(nb_buf_cstr(&data), 1, nb_buf_len(&data), output) != nb_buf_len(&data))
		return NB_ERR_IO;
	return 0;
}

int morph_sync_restore_db(const struct morph_sync_config *cfg,
			  const char *snapshot_id, const char *destination)
{
	char manifest_name[MORPH_SYNC_SNAPSHOT_ID_MAX + 8];
	struct backup_staging staging NB_CLEANUP(backup_staging_cleanup) = {0};
	char actual_hash[MORPH_SYNC_HASH_LEN];
	nb_buf parent NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf temporary NB_CLEANUP(nb_buf_cleanup) = {0};
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	cJSON *chunks;
	cJSON *expected_hash;
	cJSON *expected_size;
	struct stat st;
	FILE *output NB_CLEANUP(backup_file_cleanup) = NULL;
	int rc;

	if (!sync_db_backend_valid(cfg) || !snapshot_id_valid(snapshot_id) || !destination ||
	    !destination[0])
		return NB_ERR_INVAL;
	if (nb_file_exists(destination))
		return NB_ERR_EXISTS;
	if (snprintf(manifest_name, sizeof(manifest_name), "%s.json",
		     snapshot_id) >= (int)sizeof(manifest_name))
		return NB_ERR_NAME_TOO_LONG;
	rc = manifest_load(cfg, manifest_name, &root);
	if (rc != 0)
		return rc;
	chunks = cJSON_GetObjectItem(root, "chunks");
	expected_hash = cJSON_GetObjectItem(root, "database_hash");
	expected_size = cJSON_GetObjectItem(root, "database_size");
	if (!cJSON_IsArray(chunks) || !cJSON_IsString(expected_hash) ||
	    !cJSON_IsNumber(expected_size)) {
		return NB_ERR_PARSE;
	}
	rc = ensure_parent(destination);
	if (rc != 0)
		return rc;
	rc = nb_path_dirname(&parent, destination);
	if (rc == 0)
		rc = nb_file_temp_in(&temporary, nb_buf_cstr(&parent), NULL, NULL);
	if (rc != 0)
		return rc;
	if (nb_buf_len(&temporary) >= sizeof(staging.snapshot)) {
		(void)nb_file_remove(nb_buf_cstr(&temporary), NULL);
		return NB_ERR_NAME_TOO_LONG;
	}
	memcpy(staging.snapshot, nb_buf_cstr(&temporary), nb_buf_len(&temporary) + 1);
	rc = staging_path(cfg, ".chunk", staging.chunk);
	if (rc != 0)
		return rc;
	output = fopen(staging.snapshot, "wb");
	if (!output) {
		return nb_status_from_errno(errno, NULL);
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
		return NB_ERR_IO;
	if (stat(staging.snapshot, &st) != 0 ||
	    (int64_t)st.st_size != (int64_t)expected_size->valuedouble) {
		nb_log_write(cfg ? cfg->logger : NULL, NB_LOG_ERROR,
			     "database restore size mismatch for %s", snapshot_id);
		return NB_ERR_FORMAT;
	}
	rc = sync_hash_file(staging.snapshot, actual_hash);
	if (rc != 0)
		return rc;
	if (strcmp(actual_hash, expected_hash->valuestring) != 0 ||
	    !sync_db_quick_check(staging.snapshot)) {
		nb_log_write(cfg ? cfg->logger : NULL, NB_LOG_ERROR,
			     "database restore integrity check failed for %s", snapshot_id);
		return NB_ERR_FORMAT;
	}
	return nb_file_copy(staging.snapshot, destination,
			    &(nb_file_copy_options){.preserve_mode = true, .nofollow_source = true},
			    NULL);
}

static int store_delete(const struct morph_sync_config *cfg, const char *rel)
{
	char path[PATH_MAX];

	if (cfg->remote_backend)
		return cfg->remote_backend->delete_file(
			cfg->remote_backend->user_data, rel);
	if (store_path(cfg, rel, path) != 0)
		return NB_ERR_NAME_TOO_LONG;
	if (unlink(path) != 0 && errno != ENOENT)
		return nb_status_from_errno(errno, NULL);
	return 0;
}

struct retention_scan {
	const struct morph_sync_config *cfg;
	nb_vec *snapshots;
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
	stored = nb_vec_push(scan->snapshots);
	if (!stored) {
		cJSON_Delete(root);
		return NB_ERR_NOMEM;
	}
	memset(stored, 0, sizeof(*stored));
	(void)nb_str_copy_to(nb_str_from_parts((name), strnlen((name), sizeof(stored->name) - 1)),
			     stored->name, sizeof(stored->name));
	rc = backup_from_json(root, &stored->backup);
	cJSON_Delete(root);
	if (rc != 0)
		scan->snapshots->len--;
	return rc;
}

static int retention_remote_cb(const char *name, int is_dir, void *user_data)
{
	struct retention_scan *scan = user_data;

	if (!scan || scan->rc != 0 || is_dir)
		return scan ? scan->rc : NB_ERR_INVAL;
	scan->rc = retention_add_name(scan, name);
	return scan->rc;
}

static int retention_collect(const struct morph_sync_config *cfg, nb_vec *snapshots)
{
	int directory_scan_status = NB_OK;

	struct retention_scan scan;
	char directory[PATH_MAX];
	nb_dir_listing names NB_CLEANUP(nb_dir_listing_destroy) = {0};
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
	rc = ((directory_scan_status = nb_dir_scan(
		       directory, NB_DIR_SCAN_FILES | NB_DIR_SCAN_SORT | NB_DIR_SCAN_FOLLOW_LINKS,
		       &names, NULL)) != NB_OK
		      ? directory_scan_status
	      : names.names.len > INT_MAX ? NB_ERR_RANGE
					  : (count = (int)names.names.len, NB_OK));
	if (rc == NB_ERR_NOT_FOUND)
		return 0;
	for (int i = 0; rc == 0 && i < count; i++)
		rc = retention_add_name(&scan, nb_dir_listing_name(&names, (size_t)(i)));
	nb_dir_listing_destroy(&names);
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

static int live_chunks_add(const struct morph_sync_config *cfg, const char *manifest_name,
			   nb_map *live)
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
		return NB_ERR_PARSE;
	}
	cJSON_ArrayForEach(item, chunks) {
		cJSON *hash = cJSON_GetObjectItem(item, "hash");
		if (!cJSON_IsString(hash)) {
			rc = NB_ERR_PARSE;
			break;
		}
		if (!nb_map_put_cstr(live, hash->valuestring, (void *)(uintptr_t)1, NULL)) {
			rc = NB_ERR_NOMEM;
			break;
		}
	}
	cJSON_Delete(root);
	return rc;
}

struct object_gc {
	const struct morph_sync_config *cfg;
	nb_map *live;
	int64_t cutoff;
	char directory[PATH_MAX];
	int rc;
};

static int object_gc_one(struct object_gc *gc, const char *name)
{
	struct morph_sync_backend_stat st;
	char rel[PATH_MAX];
	int rc;

	if (nb_map_has_cstr(gc->live, name))
		return 0;
	if (snprintf(rel, sizeof(rel), "%s/%s", gc->directory, name) >=
	    (int)sizeof(rel))
		return NB_ERR_NAME_TOO_LONG;
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
		return gc ? gc->rc : NB_ERR_INVAL;
	gc->rc = object_gc_one(gc, name);
	return gc->rc;
}

static int object_prefix_cb(const char *name, int is_dir, void *user_data)
{
	struct object_gc *gc = user_data;
	struct object_gc child;

	if (!gc || gc->rc != 0 || !is_dir)
		return gc ? gc->rc : NB_ERR_INVAL;
	child = *gc;
	if (snprintf(child.directory, sizeof(child.directory), "%s/%s",
		     DB_OBJECT_DIR, name) >= (int)sizeof(child.directory))
		return NB_ERR_NAME_TOO_LONG;
	gc->rc = gc->cfg->remote_backend->list(
		gc->cfg->remote_backend->user_data, child.directory,
		object_file_cb, &child);
	if (gc->rc == 0)
		gc->rc = child.rc;
	return gc->rc;
}

static int object_gc_local(struct object_gc *gc)
{
	int directory_scan_status = NB_OK;
	int object_scan_status = NB_OK;

	char root[PATH_MAX];
	nb_dir_listing prefixes NB_CLEANUP(nb_dir_listing_destroy) = {0};
	int prefix_count = 0;
	int rc;

	rc = store_path(gc->cfg, DB_OBJECT_DIR, root);
	if (rc != 0)
		return rc;
	rc = ((directory_scan_status = nb_dir_scan(
		       root, NB_DIR_SCAN_DIRS | NB_DIR_SCAN_HIDDEN | NB_DIR_SCAN_FOLLOW_LINKS,
		       &prefixes, NULL)) != NB_OK
		      ? directory_scan_status
	      : prefixes.names.len > INT_MAX ? NB_ERR_RANGE
					     : (prefix_count = (int)prefixes.names.len, NB_OK));
	if (rc == NB_ERR_NOT_FOUND)
		return 0;
	for (int i = 0; rc == 0 && i < prefix_count; i++) {
		char directory[PATH_MAX];
		nb_dir_listing names NB_CLEANUP(nb_dir_listing_destroy) = {0};
		int count = 0;
		if (snprintf(gc->directory, sizeof(gc->directory), "%s/%s", DB_OBJECT_DIR,
			     nb_dir_listing_name(&prefixes, (size_t)(i))) >=
		    (int)sizeof(gc->directory)) {
			rc = NB_ERR_NAME_TOO_LONG;
			break;
		}
		rc = store_path(gc->cfg, gc->directory, directory);
		if (rc == 0)
			rc = ((object_scan_status =
				       nb_dir_scan(directory,
						   NB_DIR_SCAN_FILES | NB_DIR_SCAN_SORT |
							   NB_DIR_SCAN_FOLLOW_LINKS,
						   &names, NULL)) != NB_OK
				      ? object_scan_status
			      : names.names.len > INT_MAX ? NB_ERR_RANGE
							  : (count = (int)names.names.len, NB_OK));
		for (int j = 0; rc == 0 && j < count; j++)
			rc = object_gc_one(gc, nb_dir_listing_name(&names, (size_t)(j)));
		nb_dir_listing_destroy(&names);
	}
	nb_dir_listing_destroy(&prefixes);
	return rc;
}

int sync_db_backup_cleanup(const struct morph_sync_config *cfg)
{
	nb_vec snapshots;
	nb_map *live = NULL;
	struct stored_snapshot *items;
	struct object_gc gc;
	int64_t cutoff;
	int rc;

	if (!sync_db_backend_valid(cfg))
		return NB_ERR_INVAL;
	if (cfg->retention_days <= 0)
		return 0;
	cutoff = (int64_t)time(NULL) -
		((int64_t)cfg->retention_days * 24 * 60 * 60);
	rc = (nb_vec_init(&snapshots, sizeof(struct stored_snapshot)),
	      nb_vec_reserve(&snapshots, 8) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = retention_collect(cfg, &snapshots);
	if (rc != 0) {
		nb_vec_cleanup(&snapshots);
		return rc;
	}
	items = ((void *)snapshots.data);
	for (size_t i = 0; i < snapshots.len; i++) {
		char rel[PATH_MAX];
		if (items[i].backup.created_at >= cutoff ||
		    snapshot_is_latest(items, snapshots.len, i))
			continue;
		if (snprintf(rel, sizeof(rel), "%s/%s", DB_SNAPSHOT_DIR,
			     items[i].name) >= (int)sizeof(rel)) {
			rc = NB_ERR_NAME_TOO_LONG;
			break;
		}
		rc = store_delete(cfg, rel);
		if (rc != 0)
			break;
		items[i].removed = 1;
	}
	if (rc == 0) {
		live = nb_map_new_with_cap(64);
		if (!live)
			rc = NB_ERR_NOMEM;
	}
	if (rc != 0) {
		nb_vec_cleanup(&snapshots);
		return rc;
	}
	for (size_t i = 0; rc == 0 && i < snapshots.len; i++) {
		if (!items[i].removed)
			rc = live_chunks_add(cfg, items[i].name, live);
	}
	memset(&gc, 0, sizeof(gc));
	gc.cfg = cfg;
	gc.live = live;
	gc.cutoff = cutoff;
	if (rc == 0 && cfg->remote_backend)
		rc = cfg->remote_backend->list(cfg->remote_backend->user_data,
			DB_OBJECT_DIR, object_prefix_cb, &gc);
	else if (rc == 0)
		rc = object_gc_local(&gc);
	if (rc == 0)
		rc = gc.rc;
	nb_map_free(live);
	nb_vec_cleanup(&snapshots);
	return rc;
}
