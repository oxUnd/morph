#include "sync/sync.h"
#include "sync/db_backup.h"
#include "nb/vec.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/log.h"
#include "content_hash.h"
#include <sqlite3.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#define SYNC_META_DIR ".morph-sync"
#define SYNC_DATA_DIR "data"
#define SYNC_MANIFEST "manifest.db"
#define SYNC_STAGING "staging"
#define SYNC_TRASH "trash"

struct sync_file {
	char path[PATH_MAX];
	char full[PATH_MAX];
	char hash[MORPH_SYNC_HASH_LEN];
	int exists;
	int64_t size;
	int64_t mtime;
};

struct sync_pair {
	char path[PATH_MAX];
	struct sync_file local;
	struct sync_file remote;
};

struct manifest_entry {
	int found;
	int local_exists;
	int remote_exists;
	int tombstone;
	int64_t local_size;
	int64_t remote_size;
	int64_t local_mtime;
	int64_t remote_mtime;
	char local_hash[MORPH_SYNC_HASH_LEN];
	char remote_hash[MORPH_SYNC_HASH_LEN];
};

static void sync_database_cleanup(sqlite3 **database)
{
	if (*database)
		sqlite3_close(*database);
	*database = NULL;
}

static void sync_stmt_cleanup(sqlite3_stmt **stmt)
{
	sqlite3_finalize(*stmt);
	*stmt = NULL;
}

static int ensure_parent_dir(const char *path)
{
	char dir[PATH_MAX];
	char *slash;

	if (!path)
		return NB_ERR_INVAL;
	(void)nb_str_copy_to(nb_str_from_parts((path), strnlen((path), sizeof(dir) - 1)), dir,
			     sizeof(dir));
	dir[sizeof(dir) - 1] = '\0';
	slash = strrchr(dir, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	if (!dir[0])
		return 0;
	return nb_file_mkdir(dir, true, NULL);
}

static int sync_path_join(char *dst, size_t size, const char *a,
			  const char *b)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;

	int rc;

	rc = ((file_status_1 = nb_path_concat(&file_value_1, a, b)) != NB_OK ? file_status_1
	      : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), dst, size)) ==
			      NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_1);
	if (rc != 0)
		return rc;
	return 0;
}

static int sync_has_remote_backend(const struct morph_sync_config *cfg)
{
	return cfg && cfg->remote_backend &&
		cfg->remote_backend->stat &&
		cfg->remote_backend->list &&
		cfg->remote_backend->copy_from_local &&
		cfg->remote_backend->copy_to_local &&
		cfg->remote_backend->delete_file;
}

static int path_has_suffix(const char *path, const char *suffix)
{
	size_t plen;
	size_t slen;

	if (!path || !suffix)
		return 0;
	plen = strlen(path);
	slen = strlen(suffix);
	return plen >= slen && strcmp(path + plen - slen, suffix) == 0;
}

static int should_skip_rel(const char *rel)
{
	if (!rel || !rel[0])
		return 1;
	if (strncmp(rel, SYNC_META_DIR "/", strlen(SYNC_META_DIR) + 1) == 0)
		return 1;
	if (strncmp(rel, "log/", 4) == 0 || strcmp(rel, "log") == 0)
		return 1;
	if (strncmp(rel, "runtime/", 8) == 0 || strcmp(rel, "runtime") == 0)
		return 1;
	if (path_has_suffix(rel, "-wal") || path_has_suffix(rel, "-shm"))
		return 1;
	return 0;
}

static int is_db_rel(const char *rel)
{
	return path_has_suffix(rel, ".db");
}

static void sync_device_id(char *buf, size_t size)
{
	if (!buf || size == 0)
		return;
	if (gethostname(buf, size) != 0 || !buf[0])
		snprintf(buf, size, "device-%u", (unsigned)getpid());
	buf[size - 1] = '\0';
}

static struct sync_pair *find_pair(nb_vec *pairs, const char *rel)
{
	struct sync_pair *items;

	items = ((void *)pairs->data);
	for (size_t i = 0; i < pairs->len; i++) {
		if (strcmp(items[i].path, rel) == 0)
			return &items[i];
	}
	return NULL;
}

static int add_file(nb_vec *pairs, const char *root, const char *rel, int remote_side)
{
	struct sync_pair *pair;
	struct sync_file *file;
	struct stat st;
	int rc;

	if (should_skip_rel(rel))
		return 0;
	pair = find_pair(pairs, rel);
	if (!pair) {
		pair = nb_vec_push(pairs);
		if (!pair)
			return NB_ERR_NOMEM;
		memset(pair, 0, sizeof(*pair));
		(void)nb_str_copy_to(
			nb_str_from_parts((rel), strnlen((rel), sizeof(pair->path) - 1)),
			pair->path, sizeof(pair->path));
	}
	file = remote_side ? &pair->remote : &pair->local;
	(void)nb_str_copy_to(nb_str_from_parts((rel), strnlen((rel), sizeof(file->path) - 1)),
			     file->path, sizeof(file->path));
	rc = sync_path_join(file->full, sizeof(file->full), root, rel);
	if (rc != 0)
		return rc;
	rc = stat(file->full, &st);
	if (rc != 0) {
		return NB_ERR_NOT_FOUND;
	}
	file->exists = 1;
	file->size = (int64_t)st.st_size;
	file->mtime = (int64_t)st.st_mtime;
	return sync_hash_file(file->full, file->hash);
}

static int add_remote_backend_file(nb_vec *pairs, const struct morph_sync_config *cfg,
				   const char *rel)
{
	struct morph_sync_backend_stat st;
	struct sync_pair *pair;
	struct sync_file *file;
	int rc;

	if (should_skip_rel(rel))
		return 0;
	memset(&st, 0, sizeof(st));
	rc = cfg->remote_backend->stat(cfg->remote_backend->user_data, rel, &st);
	if (rc != 0)
		return rc;
	if (!st.exists || st.is_dir)
		return 0;
	pair = find_pair(pairs, rel);
	if (!pair) {
		pair = nb_vec_push(pairs);
		if (!pair)
			return NB_ERR_NOMEM;
		memset(pair, 0, sizeof(*pair));
		(void)nb_str_copy_to(
			nb_str_from_parts((rel), strnlen((rel), sizeof(pair->path) - 1)),
			pair->path, sizeof(pair->path));
	}
	file = &pair->remote;
	(void)nb_str_copy_to(nb_str_from_parts((rel), strnlen((rel), sizeof(file->path) - 1)),
			     file->path, sizeof(file->path));
	(void)nb_str_copy_to(nb_str_from_parts((rel), strnlen((rel), sizeof(file->full) - 1)),
			     file->full, sizeof(file->full));
	file->exists = 1;
	file->size = st.size;
	file->mtime = st.mtime;
	(void)nb_str_copy_to(
		nb_str_from_parts((st.hash), strnlen((st.hash), sizeof(file->hash) - 1)),
		file->hash, sizeof(file->hash));
	return 0;
}

static int refresh_file(struct sync_file *file)
{
	struct stat st;
	int rc;

	if (!file || !file->full[0])
		return NB_ERR_INVAL;
	rc = stat(file->full, &st);
	if (rc != 0) {
		if (errno != ENOENT)
			return nb_status_from_errno(errno, NULL);
		file->exists = 0;
		file->hash[0] = '\0';
		file->size = 0;
		file->mtime = 0;
		return 0;
	}
	file->exists = 1;
	file->size = (int64_t)st.st_size;
	file->mtime = (int64_t)st.st_mtime;
	return sync_hash_file(file->full, file->hash);
}

static int refresh_remote_backend_file(const struct morph_sync_config *cfg,
				       struct sync_file *file)
{
	struct morph_sync_backend_stat st;
	int rc;

	if (!cfg || !file || !file->path[0])
		return NB_ERR_INVAL;
	memset(&st, 0, sizeof(st));
	rc = cfg->remote_backend->stat(cfg->remote_backend->user_data,
				       file->path, &st);
	if (rc != 0)
		return rc;
	if (!st.exists || st.is_dir) {
		file->exists = 0;
		file->hash[0] = '\0';
		file->size = 0;
		file->mtime = 0;
		return 0;
	}
	file->exists = 1;
	file->size = st.size;
	file->mtime = st.mtime;
	(void)nb_str_copy_to(
		nb_str_from_parts((st.hash), strnlen((st.hash), sizeof(file->hash) - 1)),
		file->hash, sizeof(file->hash));
	return 0;
}

struct remote_scan_ctx {
	nb_vec *pairs;
	const struct morph_sync_config *cfg;
	char rel_dir[PATH_MAX];
	int rc;
};

static int scan_remote_backend_dir(nb_vec *pairs, const struct morph_sync_config *cfg,
				   const char *rel_dir);

static int remote_scan_list_cb(const char *name, int is_dir, void *user_data)
{
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_2 = NB_OK;

	struct remote_scan_ctx *ctx = user_data;
	char child_rel[PATH_MAX];
	int rc;

	if (!ctx || ctx->rc != 0 || !name || !name[0])
		return ctx ? ctx->rc : NB_ERR_INVAL;
	if (ctx->rel_dir[0]) {
		rc = ((file_status_2 = nb_path_concat(&file_value_2, ctx->rel_dir, name)) != NB_OK
			      ? file_status_2
		      : (file_status_2 = nb_str_copy_to(nb_buf_str(&file_value_2), child_rel,
							sizeof(child_rel))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_2);
	} else {
		(void)nb_str_copy_to(
			nb_str_from_parts((name), strnlen((name), sizeof(child_rel) - 1)),
			child_rel, sizeof(child_rel));
		child_rel[sizeof(child_rel) - 1] = '\0';
		rc = 0;
	}
	if (rc != 0 || should_skip_rel(child_rel))
		return 0;
	if (is_dir)
		rc = scan_remote_backend_dir(ctx->pairs, ctx->cfg, child_rel);
	else
		rc = add_remote_backend_file(ctx->pairs, ctx->cfg, child_rel);
	if (rc != 0)
		ctx->rc = rc;
	return 0;
}

static int scan_dir(nb_vec *pairs, const char *root, const char *rel_dir, int remote_side)
{
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_3 = NB_OK;

	char full[PATH_MAX];
	nb_dir *dir NB_CLEANUP(nb_dir_cleanup) = NULL;
	nb_err directory_error = {0};
	nb_dir_entry entry;
	const nb_dir_entry *ent = &entry;
	int rc;

	rc = sync_path_join(full, sizeof(full), root, rel_dir);
	if (rc != 0)
		return rc;
	dir = nb_dir_open(full, &directory_error);
	if (!dir && directory_error.code == NB_ERR_NOT_FOUND)
		return 0;
	if (!dir)
		return directory_error.code;
	while (nb_dir_next(dir, &entry) != NULL) {
		char child_rel[PATH_MAX];
		char child_full[PATH_MAX];
		struct stat st;
		if (strcmp(ent->name, ".") == 0 ||
		    strcmp(ent->name, "..") == 0)
			continue;
		if (rel_dir[0]) {
			rc = ((file_status_3 =
				       nb_path_concat(&file_value_3, rel_dir, ent->name)) != NB_OK
				      ? file_status_3
			      : (file_status_3 = nb_str_copy_to(nb_buf_str(&file_value_3),
								child_rel, sizeof(child_rel))) ==
					      NB_ERR_RANGE
				      ? NB_ERR_NAME_TOO_LONG
				      : file_status_3);
		} else {
			rc = nb_str_copy_to(nb_str_from_cstr(ent->name), child_rel,
					    sizeof(child_rel));
			if (rc == NB_ERR_RANGE)
				rc = NB_ERR_NAME_TOO_LONG;
		}
		if (rc != 0 || should_skip_rel(child_rel))
			continue;
		rc = sync_path_join(child_full, sizeof(child_full),
				    root, child_rel);
		if (rc != 0)
			break;
		if (stat(child_full, &st) != 0) {
			rc = (nb_status_from_errno(errno, NULL));
			break;
		}
		if (S_ISDIR(st.st_mode)) {
			rc = scan_dir(pairs, root, child_rel, remote_side);
			if (rc != 0)
				break;
		} else if (S_ISREG(st.st_mode)) {
			rc = add_file(pairs, root, child_rel, remote_side);
			if (rc != 0)
				break;
		}
	}
	return rc != NB_OK ? rc : nb_dir_status(dir);
}

static int scan_remote_backend_dir(nb_vec *pairs, const struct morph_sync_config *cfg,
				   const char *rel_dir)
{
	struct remote_scan_ctx ctx;
	int rc;

	if (!cfg || !cfg->remote_backend || !cfg->remote_backend->list)
		return NB_ERR_INVAL;
	memset(&ctx, 0, sizeof(ctx));
	ctx.pairs = pairs;
	ctx.cfg = cfg;
	if (rel_dir)
		(void)nb_str_copy_to(
			nb_str_from_parts((rel_dir), strnlen((rel_dir), sizeof(ctx.rel_dir) - 1)),
			ctx.rel_dir, sizeof(ctx.rel_dir));
	rc = cfg->remote_backend->list(cfg->remote_backend->user_data,
				       rel_dir ? rel_dir : "",
				       remote_scan_list_cb, &ctx);
	if (rc != 0)
		return rc;
	return ctx.rc;
}

static int scan_remote_backend_include(nb_vec *pairs, const struct morph_sync_config *cfg,
				       const char *include)
{
	struct morph_sync_backend_stat st;
	int rc;

	if (!include || !include[0])
		return 0;
	memset(&st, 0, sizeof(st));
	rc = cfg->remote_backend->stat(cfg->remote_backend->user_data,
				       include, &st);
	if (rc != 0)
		return rc;
	if (!st.exists)
		return 0;
	if (st.is_dir)
		return scan_remote_backend_dir(pairs, cfg, include);
	return add_remote_backend_file(pairs, cfg, include);
}

static int scan_include(nb_vec *pairs, const char *root, const char *include, int remote_side)
{
	char full[PATH_MAX];
	struct stat st;
	int rc;

	rc = sync_path_join(full, sizeof(full), root, include);
	if (rc != 0)
		return rc;
	rc = stat(full, &st);
	if (rc != 0 && errno == ENOENT)
		return 0;
	if (rc != 0)
		return nb_status_from_errno(errno, NULL);
	if (S_ISDIR(st.st_mode))
		return scan_dir(pairs, root, include, remote_side);
	return add_file(pairs, root, include, remote_side);
}

static int output_relative(const char *rel)
{
	return strcmp(rel, "output") == 0 || strncmp(rel, "output/", 7) == 0;
}

static int local_path(const struct morph_sync_config *cfg, const char *rel,
		      char *out, size_t size)
{
	if (cfg->output_dir[0] && output_relative(rel))
		return sync_path_join(out, size, cfg->output_dir,
				      rel[6] ? rel + 7 : "");
	return sync_path_join(out, size, cfg->source_dir, rel);
}

static int scan_output(nb_vec *pairs, const struct morph_sync_config *cfg, const char *include)
{
	nb_vec files;
	int rc = (nb_vec_init(&files, sizeof(struct sync_pair)),
		  nb_vec_reserve(&files, 16) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = include[6] ? scan_include(&files, cfg->output_dir, include + 7, 0)
		: scan_dir(&files, cfg->output_dir, "", 0);
	for (size_t i = 0; rc == 0 && i < files.len; i++) {
		struct sync_pair *file = &((struct sync_pair *)((void *)files.data))[i];
		struct sync_pair *pair;
		char path[PATH_MAX];
		rc = sync_path_join(path, sizeof(path), "output", file->path);
		if (rc != 0)
			break;
		pair = find_pair(pairs, path);
		if (!pair) {
			pair = nb_vec_push(pairs);
			if (!pair) {
				rc = (NB_ERR_NOMEM);
				break;
			}
			memset(pair, 0, sizeof(*pair));
			(void)nb_str_copy_to(
				nb_str_from_parts((path), strnlen((path), sizeof(pair->path) - 1)),
				pair->path, sizeof(pair->path));
		}
		pair->local = file->local;
		(void)nb_str_copy_to(
			nb_str_from_parts((path), strnlen((path), sizeof(pair->local.path) - 1)),
			pair->local.path, sizeof(pair->local.path));
	}
	nb_vec_cleanup(&files);
	return rc;
}

static int scan_side(nb_vec *pairs, const char *root, const struct morph_sync_config *cfg,
		     int remote_side)
{
	if (!root || !root[0])
		return NB_ERR_INVAL;
	for (int i = 0; i < cfg->include_count; i++) {
		int rc;
		if (remote_side && sync_has_remote_backend(cfg))
			rc = scan_remote_backend_include(pairs, cfg,
							 cfg->include[i]);
		else if (!remote_side && cfg->output_dir[0] &&
			 output_relative(cfg->include[i]))
			rc = scan_output(pairs, cfg, cfg->include[i]);
		else
			rc = scan_include(pairs, root, cfg->include[i],
					  remote_side);
		if (rc != 0)
			return rc;
	}
	return 0;
}

static int manifest_open(sqlite3 **out, const char *sync_dir)
{
	char meta[PATH_MAX];
	char path[PATH_MAX];
	int rc;

	rc = sync_path_join(meta, sizeof(meta), sync_dir, SYNC_META_DIR);
	if (rc != 0)
		return rc;
	rc = nb_file_mkdir(meta, true, NULL);
	if (rc != 0)
		return rc;
	rc = sync_path_join(path, sizeof(path), meta, SYNC_MANIFEST);
	if (rc != 0)
		return rc;
	if (sqlite3_open_v2(path, out, SQLITE_OPEN_READWRITE |
	    SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	rc = sqlite3_exec(*out,
		"CREATE TABLE IF NOT EXISTS entries("
		"path TEXT PRIMARY KEY,"
		"local_hash TEXT,"
		"remote_hash TEXT,"
		"local_exists INTEGER NOT NULL DEFAULT 0,"
		"remote_exists INTEGER NOT NULL DEFAULT 0,"
		"local_size INTEGER NOT NULL DEFAULT 0,"
		"remote_size INTEGER NOT NULL DEFAULT 0,"
		"local_mtime INTEGER NOT NULL DEFAULT 0,"
		"remote_mtime INTEGER NOT NULL DEFAULT 0,"
		"tombstone INTEGER NOT NULL DEFAULT 0,"
		"last_device TEXT,"
		"updated_at INTEGER NOT NULL);"
		"CREATE TABLE IF NOT EXISTS conflicts("
		"id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"path TEXT NOT NULL,"
		"local_hash TEXT,"
		"remote_hash TEXT,"
		"created_at INTEGER NOT NULL,"
		"resolved_at INTEGER NOT NULL DEFAULT 0);"
		"CREATE TABLE IF NOT EXISTS trash("
		"id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"path TEXT NOT NULL,"
		"trash_path TEXT NOT NULL,"
		"created_at INTEGER NOT NULL);"
		"CREATE TABLE IF NOT EXISTS tombstones("
		"path TEXT PRIMARY KEY,"
		"deleted_at INTEGER NOT NULL,"
		"device_id TEXT);",
		NULL, NULL, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	(void)sqlite3_exec(*out,
		"ALTER TABLE entries ADD COLUMN local_size INTEGER NOT NULL DEFAULT 0",
		NULL, NULL, NULL);
	(void)sqlite3_exec(*out,
		"ALTER TABLE entries ADD COLUMN remote_size INTEGER NOT NULL DEFAULT 0",
		NULL, NULL, NULL);
	(void)sqlite3_exec(*out,
		"ALTER TABLE entries ADD COLUMN local_mtime INTEGER NOT NULL DEFAULT 0",
		NULL, NULL, NULL);
	(void)sqlite3_exec(*out,
		"ALTER TABLE entries ADD COLUMN remote_mtime INTEGER NOT NULL DEFAULT 0",
		NULL, NULL, NULL);
	(void)sqlite3_exec(*out,
		"ALTER TABLE entries ADD COLUMN last_device TEXT",
		NULL, NULL, NULL);
	(void)sqlite3_exec(*out,
		"ALTER TABLE conflicts ADD COLUMN resolved_at INTEGER NOT NULL DEFAULT 0",
		NULL, NULL, NULL);
	return 0;
}

static int manifest_get(sqlite3 *db, const char *path,
			struct manifest_entry *out)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	memset(out, 0, sizeof(*out));
	rc = sqlite3_prepare_v2(db,
		"SELECT local_hash,remote_hash,local_exists,remote_exists,"
		"tombstone,local_size,remote_size,local_mtime,remote_mtime "
		"FROM entries WHERE path=?",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		const char *lh = (const char *)sqlite3_column_text(stmt, 0);
		const char *rh = (const char *)sqlite3_column_text(stmt, 1);
		out->found = 1;
		if (lh)
			(void)nb_str_copy_to(
				nb_str_from_parts((lh), strnlen((lh), sizeof(out->local_hash) - 1)),
				out->local_hash, sizeof(out->local_hash));
		if (rh)
			(void)nb_str_copy_to(
				nb_str_from_parts((rh),
						  strnlen((rh), sizeof(out->remote_hash) - 1)),
				out->remote_hash, sizeof(out->remote_hash));
		out->local_exists = sqlite3_column_int(stmt, 2);
		out->remote_exists = sqlite3_column_int(stmt, 3);
		out->tombstone = sqlite3_column_int(stmt, 4);
		out->local_size = sqlite3_column_int64(stmt, 5);
		out->remote_size = sqlite3_column_int64(stmt, 6);
		out->local_mtime = sqlite3_column_int64(stmt, 7);
		out->remote_mtime = sqlite3_column_int64(stmt, 8);
	}
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE || rc == SQLITE_ROW ? 0 : MORPH_ERR_DB;
}

static int manifest_put(sqlite3 *db, const struct sync_pair *pair,
			int tombstone)
{
	sqlite3_stmt *stmt = NULL;
	char device[128];
	int rc;
	int64_t now = (int64_t)time(NULL);

	sync_device_id(device, sizeof(device));
	rc = sqlite3_prepare_v2(db,
		"INSERT INTO entries(path,local_hash,remote_hash,local_exists,"
		"remote_exists,local_size,remote_size,local_mtime,remote_mtime,"
		"tombstone,last_device,updated_at) "
		"VALUES(?,?,?,?,?,?,?,?,?,?,?,?) "
		"ON CONFLICT(path) DO UPDATE SET local_hash=excluded.local_hash,"
		"remote_hash=excluded.remote_hash,"
		"local_exists=excluded.local_exists,"
		"remote_exists=excluded.remote_exists,"
		"local_size=excluded.local_size,"
		"remote_size=excluded.remote_size,"
		"local_mtime=excluded.local_mtime,"
		"remote_mtime=excluded.remote_mtime,"
		"tombstone=excluded.tombstone,"
		"last_device=excluded.last_device,"
		"updated_at=excluded.updated_at",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_text(stmt, 1, pair->path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, pair->local.exists ?
			  pair->local.hash : "", -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, pair->remote.exists ?
			  pair->remote.hash : "", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 4, pair->local.exists);
	sqlite3_bind_int(stmt, 5, pair->remote.exists);
	sqlite3_bind_int64(stmt, 6, pair->local.exists ?
			   pair->local.size : 0);
	sqlite3_bind_int64(stmt, 7, pair->remote.exists ?
			   pair->remote.size : 0);
	sqlite3_bind_int64(stmt, 8, pair->local.exists ?
			   pair->local.mtime : 0);
	sqlite3_bind_int64(stmt, 9, pair->remote.exists ?
			   pair->remote.mtime : 0);
	sqlite3_bind_int(stmt, 10, tombstone);
	sqlite3_bind_text(stmt, 11, device, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 12, now);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int manifest_conflict(sqlite3 *db, const struct sync_pair *pair)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	rc = sqlite3_prepare_v2(db,
		"INSERT INTO conflicts(path,local_hash,remote_hash,created_at)"
		" VALUES(?,?,?,?)",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_text(stmt, 1, pair->path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, pair->local.hash, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, pair->remote.hash, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 4, (int64_t)time(NULL));
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int manifest_resolve_db_conflicts(sqlite3 *db, const char *path)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	rc = sqlite3_prepare_v2(db,
		"UPDATE conflicts SET resolved_at=? WHERE path=? AND resolved_at=0",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
	sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int manifest_trash(sqlite3 *db, const char *path,
			  const char *trash_path)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	rc = sqlite3_prepare_v2(db,
		"INSERT INTO trash(path,trash_path,created_at) VALUES(?,?,?)",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, trash_path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, (int64_t)time(NULL));
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int manifest_tombstone(sqlite3 *db, const char *path)
{
	sqlite3_stmt *stmt = NULL;
	char device[128];
	int rc;

	sync_device_id(device, sizeof(device));
	rc = sqlite3_prepare_v2(db,
		"INSERT INTO tombstones(path,deleted_at,device_id) "
		"VALUES(?,?,?) ON CONFLICT(path) DO UPDATE SET "
		"deleted_at=excluded.deleted_at,device_id=excluded.device_id",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, (int64_t)time(NULL));
	sqlite3_bind_text(stmt, 3, device, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int manifest_clear_tombstone(sqlite3 *db, const char *path)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	rc = sqlite3_prepare_v2(db, "DELETE FROM tombstones WHERE path=?",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int manifest_delete_trash(sqlite3 *db, int64_t id)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	rc = sqlite3_prepare_v2(db, "DELETE FROM trash WHERE id=?",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int copy_sqlite_backup(const char *src, const char *dst)
{
	sqlite3 *from NB_CLEANUP(sync_database_cleanup) = NULL;
	sqlite3 *to NB_CLEANUP(sync_database_cleanup) = NULL;
	sqlite3_backup *backup;
	int rc;

	rc = sqlite3_open_v2(src, &from, SQLITE_OPEN_READONLY, NULL);
	if (rc != SQLITE_OK)
		return NB_ERR_INVAL;
	rc = sqlite3_open_v2(dst, &to, SQLITE_OPEN_READWRITE |
			     SQLITE_OPEN_CREATE, NULL);
	if (rc != SQLITE_OK)
		return NB_ERR_INVAL;
	backup = sqlite3_backup_init(to, "main", from, "main");
	if (!backup)
		return NB_ERR_INVAL;
	(void)sqlite3_backup_step(backup, -1);
	rc = sqlite3_backup_finish(backup);
	return rc == SQLITE_OK ? 0 : MORPH_ERR_DB;
}

static void sync_staging_cleanup(nb_buf *path)
{
	if (!nb_buf_empty(path))
		(void)nb_file_remove(nb_buf_cstr(path), NULL);
	nb_buf_cleanup(path);
}

/* SQLite inputs need a consistent snapshot before ordinary atomic publication. */
static int sync_copy_local_file(const char *src, const char *dst, const char *rel,
				int sqlite_source)
{
	nb_buf staging NB_CLEANUP(sync_staging_cleanup) = {0};
	const char *source = src;
	int rc = ensure_parent_dir(dst);

	if (rc != 0)
		return rc;
	if (sqlite_source && path_has_suffix(rel, ".db")) {
		rc = nb_file_temp(&staging, NULL);
		if (rc != 0)
			return rc;
		if (copy_sqlite_backup(src, nb_buf_cstr(&staging)) == 0)
			source = nb_buf_cstr(&staging);
	}
	return nb_file_copy(source, dst, &(nb_file_copy_options){.overwrite = true}, NULL);
}

static int copy_local_to_remote(const struct morph_sync_config *cfg,
				const char *local_path, const char *remote_rel,
				int sqlite_source)
{
	if (!sync_has_remote_backend(cfg))
		return NB_ERR_INVAL;
	return cfg->remote_backend->copy_from_local(
		cfg->remote_backend->user_data, local_path, remote_rel,
		sqlite_source);
}

static int copy_remote_to_local_atomic(const struct morph_sync_config *cfg,
				       const char *remote_rel,
				       const char *local_path)
{
	nb_buf staging NB_CLEANUP(sync_staging_cleanup) = {0};

	if (!sync_has_remote_backend(cfg))
		return NB_ERR_INVAL;
	int rc = nb_file_temp(&staging, NULL);
	if (rc == 0)
		rc = cfg->remote_backend->copy_to_local(cfg->remote_backend->user_data, remote_rel,
							nb_buf_cstr(&staging));
	if (rc == 0)
		rc = ensure_parent_dir(local_path);
	if (rc != 0)
		return rc;
	return nb_file_copy(nb_buf_cstr(&staging), local_path,
			    &(nb_file_copy_options){.overwrite = true}, NULL);
}

static int sync_copy_to_remote(const struct morph_sync_config *cfg,
			       const char *local_path, const char *remote_path,
			       const char *rel, int sqlite_source)
{
	if (sync_has_remote_backend(cfg))
		return copy_local_to_remote(cfg, local_path, rel, sqlite_source);
	return sync_copy_local_file(local_path, remote_path, rel, sqlite_source);
}

static int sync_copy_from_remote(const struct morph_sync_config *cfg,
				 const char *remote_path, const char *local_path,
				 const char *rel)
{
	if (sync_has_remote_backend(cfg))
		return copy_remote_to_local_atomic(cfg, rel, local_path);
	return sync_copy_local_file(remote_path, local_path, rel, 0);
}

static int sync_refresh_remote(const struct morph_sync_config *cfg,
			       struct sync_file *file)
{
	if (sync_has_remote_backend(cfg))
		return refresh_remote_backend_file(cfg, file);
	return refresh_file(file);
}

static void sanitize_rel(char *dst, size_t size, const char *rel)
{
	size_t pos = 0;

	for (size_t i = 0; rel && rel[i] && pos + 1 < size; i++) {
		char c = rel[i];
		dst[pos++] = (c == '/' || c == '\\') ? '_' : c;
	}
	dst[pos] = '\0';
}

static int move_to_trash(sqlite3 *db, const struct morph_sync_config *cfg,
			 const char *path, const char *rel)
{
	char meta[PATH_MAX];
	char trash_dir[PATH_MAX];
	char safe[PATH_MAX];
	nb_buf trash NB_CLEANUP(nb_buf_destroy) = {0};
	int rc;

	if (!nb_file_exists(path))
		return 0;
	rc = sync_path_join(meta, sizeof(meta), cfg->sync_dir, SYNC_META_DIR);
	if (rc != 0)
		return rc;
	rc = sync_path_join(trash_dir, sizeof(trash_dir), meta, SYNC_TRASH);
	if (rc != 0)
		return rc;
	rc = nb_file_mkdir(trash_dir, true, NULL);
	if (rc != 0)
		return rc;
	sanitize_rel(safe, sizeof(safe), rel);
	if (!nb_buf_printf(&trash, "%lld-%u-%s", (long long)time(NULL), (unsigned)getpid(), safe))
		return NB_ERR_NOMEM;
	rc = nb_path_join(&trash, trash_dir, nb_buf_cstr(&trash));
	if (rc != NB_OK)
		return rc;
	rc = nb_file_copy(path, nb_buf_cstr(&trash), &(nb_file_copy_options){.overwrite = true},
			  NULL);
	if (rc != 0)
		return rc;
	if (unlink(path) != 0)
		return nb_status_from_errno(errno, NULL);
	rc = manifest_trash(db, rel, nb_buf_cstr(&trash));
	if (rc != 0)
		return rc;
	return manifest_tombstone(db, rel);
}

static int move_remote_to_trash(sqlite3 *db,
				const struct morph_sync_config *cfg,
				const char *rel)
{
	char meta[PATH_MAX];
	char trash_dir[PATH_MAX];
	char safe[PATH_MAX];
	nb_buf trash NB_CLEANUP(nb_buf_destroy) = {0};
	int rc;

	if (!sync_has_remote_backend(cfg))
		return NB_ERR_INVAL;
	rc = sync_path_join(meta, sizeof(meta), cfg->sync_dir, SYNC_META_DIR);
	if (rc != 0)
		return rc;
	rc = sync_path_join(trash_dir, sizeof(trash_dir), meta, SYNC_TRASH);
	if (rc != 0)
		return rc;
	rc = nb_file_mkdir(trash_dir, true, NULL);
	if (rc != 0)
		return rc;
	sanitize_rel(safe, sizeof(safe), rel);
	if (!nb_buf_printf(&trash, "%lld-%u-%s", (long long)time(NULL), (unsigned)getpid(), safe))
		return NB_ERR_NOMEM;
	rc = nb_path_join(&trash, trash_dir, nb_buf_cstr(&trash));
	if (rc != NB_OK)
		return rc;
	rc = cfg->remote_backend->copy_to_local(cfg->remote_backend->user_data, rel,
						nb_buf_cstr(&trash));
	if (rc != 0)
		return rc;
	rc = cfg->remote_backend->delete_file(
		cfg->remote_backend->user_data, rel);
	if (rc != 0)
		return rc;
	rc = manifest_trash(db, rel, nb_buf_cstr(&trash));
	if (rc != 0)
		return rc;
	return manifest_tombstone(db, rel);
}

static int create_conflict(sqlite3 *db, const struct morph_sync_config *cfg,
			   const struct sync_pair *pair)
{
	nb_buf conflict NB_CLEANUP(nb_buf_destroy) = {0};
	int rc;

	if (!nb_buf_printf(&conflict, "%s.conflict.%lld.%u", pair->path, (long long)time(NULL),
			   (unsigned)getpid()))
		return NB_ERR_NOMEM;
	if (pair->remote.exists) {
		char dst[PATH_MAX];
		rc = local_path(cfg, nb_buf_cstr(&conflict), dst, sizeof(dst));
		if (rc != 0)
			return rc;
		rc = sync_copy_from_remote(cfg, pair->remote.full, dst,
					   pair->path);
		if (rc != 0)
			return rc;
	}
	if (pair->local.exists) {
		char data_root[PATH_MAX];
		char dst[PATH_MAX];
		rc = sync_path_join(data_root, sizeof(data_root), cfg->sync_dir,
				    SYNC_DATA_DIR);
		if (rc != 0)
			return rc;
		rc = sync_path_join(dst, sizeof(dst), data_root, nb_buf_cstr(&conflict));
		if (rc != 0)
			return rc;
		rc = sync_copy_to_remote(cfg, pair->local.full, dst, nb_buf_cstr(&conflict), 1);
		if (rc != 0)
			return rc;
	}
	return manifest_conflict(db, pair);
}

static int update_pair_paths(struct sync_pair *pair,
			     const struct morph_sync_config *cfg)
{
	char data_root[PATH_MAX];
	int rc;

	rc = sync_path_join(data_root, sizeof(data_root), cfg->sync_dir,
			    SYNC_DATA_DIR);
	if (rc != 0)
		return rc;
	if (!pair->local.full[0]) {
		rc = local_path(cfg, pair->path, pair->local.full,
				sizeof(pair->local.full));
		if (rc != 0)
			return rc;
	}
	if (!pair->local.path[0])
		(void)nb_str_copy_to(
			nb_str_from_parts((pair->path),
					  strnlen((pair->path), sizeof(pair->local.path) - 1)),
			pair->local.path, sizeof(pair->local.path));
	if (!pair->remote.path[0])
		(void)nb_str_copy_to(
			nb_str_from_parts((pair->path),
					  strnlen((pair->path), sizeof(pair->remote.path) - 1)),
			pair->remote.path, sizeof(pair->remote.path));
	if (!pair->remote.full[0]) {
		if (sync_has_remote_backend(cfg)) {
			(void)nb_str_copy_to(
				nb_str_from_parts(
					(pair->path),
					strnlen((pair->path), sizeof(pair->remote.full) - 1)),
				pair->remote.full, sizeof(pair->remote.full));
		} else {
			rc = sync_path_join(pair->remote.full,
					    sizeof(pair->remote.full),
					    data_root, pair->path);
			if (rc != 0)
				return rc;
		}
	}
	return 0;
}

static int sync_pair(sqlite3 *db, const struct morph_sync_config *cfg,
		     struct sync_pair *pair, struct morph_sync_status *status)
{
	struct manifest_entry old;
	int local_changed;
	int remote_changed;
	int is_db;
	int rc;

	rc = update_pair_paths(pair, cfg);
	if (rc != 0)
		return rc;
	rc = manifest_get(db, pair->path, &old);
	if (rc != 0)
		return rc;
	local_changed = pair->local.exists ?
		(!old.local_exists ||
		 strcmp(pair->local.hash, old.local_hash) != 0) :
		old.local_exists;
	remote_changed = pair->remote.exists ?
		(!old.remote_exists ||
		 strcmp(pair->remote.hash, old.remote_hash) != 0) :
		old.remote_exists;
	is_db = is_db_rel(pair->path);
	if (pair->local.exists && sync_db_is_sqlite(pair->local.full)) {
		rc = sync_db_backup_create(cfg, pair->path, pair->local.full,
					   status);
		if (rc != 0)
			return rc;
		rc = manifest_resolve_db_conflicts(db, pair->path);
		if (rc != 0)
			return rc;
		return manifest_put(db, pair, 0);
	}

	if (is_db) {
		if (pair->local.exists) {
			if (pair->remote.exists &&
			    strcmp(pair->local.hash, pair->remote.hash) != 0 &&
			    (!old.found || remote_changed)) {
				status->conflicts++;
				rc = create_conflict(db, cfg, pair);
				if (rc != 0)
					return rc;
				return manifest_put(db, pair, 0);
			}
			if (!pair->remote.exists || local_changed) {
				rc = sync_copy_to_remote(cfg, pair->local.full,
							 pair->remote.full,
							 pair->path, 1);
				if (rc != 0)
					return rc;
				status->copied++;
				rc = sync_refresh_remote(cfg, &pair->remote);
				if (rc != 0)
					return rc;
				return manifest_put(db, pair, 0);
			}
			return manifest_put(db, pair, 0);
		}
		if (pair->remote.exists) {
			status->conflicts++;
			rc = create_conflict(db, cfg, pair);
			if (rc != 0)
				return rc;
			return manifest_put(db, pair, 0);
		}
		return manifest_put(db, pair, old.tombstone);
	}

	if (pair->local.exists && pair->remote.exists) {
		if (strcmp(pair->local.hash, pair->remote.hash) == 0)
			return manifest_put(db, pair, 0);
		if (local_changed && remote_changed) {
			status->conflicts++;
			rc = create_conflict(db, cfg, pair);
			if (rc != 0)
				return rc;
			return manifest_put(db, pair, 0);
		}
		if (local_changed) {
			rc = sync_copy_to_remote(cfg, pair->local.full,
						 pair->remote.full,
						 pair->path, 1);
			if (rc != 0)
				return rc;
			status->copied++;
			rc = sync_refresh_remote(cfg, &pair->remote);
			if (rc != 0)
				return rc;
			return manifest_put(db, pair, 0);
		}
		if (remote_changed) {
			rc = sync_copy_from_remote(cfg, pair->remote.full,
						   pair->local.full,
						   pair->path);
			if (rc != 0)
				return rc;
			status->copied++;
			rc = refresh_file(&pair->local);
			if (rc != 0)
				return rc;
			return manifest_put(db, pair, 0);
		}
		return manifest_put(db, pair, 0);
	}

	if (pair->local.exists && !pair->remote.exists) {
		if (old.remote_exists && remote_changed && !local_changed) {
			rc = move_to_trash(db, cfg, pair->local.full, pair->path);
			if (rc != 0)
				return rc;
			status->deleted++;
			status->recycled++;
			pair->local.exists = 0;
			return manifest_put(db, pair, 1);
		}
		if (old.remote_exists && remote_changed && local_changed) {
			status->conflicts++;
			rc = create_conflict(db, cfg, pair);
			if (rc != 0)
				return rc;
			return manifest_put(db, pair, 0);
		}
		rc = sync_copy_to_remote(cfg, pair->local.full,
					 pair->remote.full, pair->path, 1);
		if (rc != 0)
			return rc;
		status->copied++;
		rc = sync_refresh_remote(cfg, &pair->remote);
		if (rc != 0)
			return rc;
		return manifest_put(db, pair, 0);
	}

	if (!pair->local.exists && pair->remote.exists) {
		if (old.local_exists && local_changed && !remote_changed) {
			if (sync_has_remote_backend(cfg))
				rc = move_remote_to_trash(db, cfg, pair->path);
			else
				rc = move_to_trash(db, cfg, pair->remote.full,
						   pair->path);
			if (rc != 0)
				return rc;
			status->deleted++;
			status->recycled++;
			pair->remote.exists = 0;
			return manifest_put(db, pair, 1);
		}
		if (old.local_exists && local_changed && remote_changed) {
			status->conflicts++;
			rc = create_conflict(db, cfg, pair);
			if (rc != 0)
				return rc;
			return manifest_put(db, pair, 0);
		}
		rc = sync_copy_from_remote(cfg, pair->remote.full,
					   pair->local.full, pair->path);
		if (rc != 0)
			return rc;
		status->copied++;
		rc = refresh_file(&pair->local);
		if (rc != 0)
			return rc;
		return manifest_put(db, pair, 0);
	}
	return manifest_put(db, pair, old.tombstone);
}

static int prepare_dirs(const struct morph_sync_config *cfg)
{
	char data[PATH_MAX];
	char meta[PATH_MAX];
	char staging[PATH_MAX];
	char trash[PATH_MAX];
	int rc;

	rc = nb_file_mkdir(cfg->sync_dir, true, NULL);
	if (rc != 0)
		return rc;
	rc = sync_path_join(data, sizeof(data), cfg->sync_dir, SYNC_DATA_DIR);
	if (rc != 0)
		return rc;
	rc = nb_file_mkdir(data, true, NULL);
	if (rc != 0)
		return rc;
	rc = sync_path_join(meta, sizeof(meta), cfg->sync_dir, SYNC_META_DIR);
	if (rc != 0)
		return rc;
	rc = nb_file_mkdir(meta, true, NULL);
	if (rc != 0)
		return rc;
	rc = sync_path_join(staging, sizeof(staging), meta, SYNC_STAGING);
	if (rc != 0)
		return rc;
	rc = nb_file_mkdir(staging, true, NULL);
	if (rc != 0)
		return rc;
	rc = sync_path_join(trash, sizeof(trash), meta, SYNC_TRASH);
	if (rc != 0)
		return rc;
	return nb_file_mkdir(trash, true, NULL);
}

static int count_table(sqlite3 *db, const char *table)
{
	char sql[128];
	sqlite3_stmt *stmt = NULL;
	int count = 0;

	snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
		return 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

static int count_unresolved_conflicts(sqlite3 *db)
{
	sqlite3_stmt *stmt = NULL;
	int count = 0;

	if (sqlite3_prepare_v2(db,
	    "SELECT COUNT(*) FROM conflicts WHERE resolved_at=0", -1,
	    &stmt, NULL) != SQLITE_OK)
		return 0;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

static int cleanup_trash(sqlite3 *db, const struct morph_sync_config *cfg)
{
	sqlite3_stmt *stmt = NULL;
	int64_t cutoff;
	int rc;

	if (!db || !cfg || cfg->retention_days <= 0)
		return 0;
	cutoff = (int64_t)time(NULL) -
		((int64_t)cfg->retention_days * 24 * 60 * 60);
	rc = sqlite3_prepare_v2(db,
		"SELECT id,trash_path FROM trash WHERE created_at<?",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, cutoff);
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
		const char *path = (const char *)sqlite3_column_text(stmt, 1);
		if (path && nb_file_exists(path)) {
			(void)unlink(path);
		}
		{
			sqlite3_stmt *del = NULL;
			if (sqlite3_prepare_v2(db, "DELETE FROM trash WHERE id=?",
			    -1, &del, NULL) == SQLITE_OK) {
				sqlite3_bind_int64(del, 1, id);
				(void)sqlite3_step(del);
			}
			sqlite3_finalize(del);
		}
	}
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int sync_once_collect(const struct morph_sync_config *cfg,
		    struct morph_sync_status *status)
{
	sqlite3 *manifest NB_CLEANUP(sync_database_cleanup) = NULL;
	nb_vec pairs NB_CLEANUP(nb_vec_cleanup) = {0};
	char data_root[PATH_MAX];
	int rc;

	rc = prepare_dirs(cfg);
	if (rc != 0)
		return rc;
	rc = manifest_open(&manifest, cfg->sync_dir);
	if (rc != 0)
		return rc;
	rc = (nb_vec_init(&pairs, sizeof(struct sync_pair)),
	      nb_vec_reserve(&pairs, 64) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = sync_path_join(data_root, sizeof(data_root), cfg->sync_dir,
			    SYNC_DATA_DIR);
	if (rc != 0)
		return rc;
	rc = scan_side(&pairs, cfg->source_dir, cfg, 0);
	if (rc != 0)
		return rc;
	rc = scan_side(&pairs, data_root, cfg, 1);
	if (rc != 0)
		return rc;
	for (size_t i = 0; i < pairs.len; i++) {
		struct sync_pair *pair = &((struct sync_pair *)((void *)pairs.data))[i];
		rc = sync_pair(manifest, cfg, pair, status);
		if (rc != 0)
			return rc;
	}
	rc = cleanup_trash(manifest, cfg);
	if (rc != 0)
		return rc;
	rc = sync_db_backup_cleanup(cfg);
	if (rc != 0)
		return rc;
	status->last_run_at = (int64_t)time(NULL);
	status->conflicts = count_unresolved_conflicts(manifest);
	status->recycled = count_table(manifest, "trash");

	return rc;
}

static int sync_once_locked(const struct morph_sync_config *cfg,
			    struct morph_sync_status *status)
{
	struct morph_sync_status local_status;

	if (!cfg || !cfg->source_dir[0] || !cfg->sync_dir[0])
		return NB_ERR_INVAL;
	if (!status)
		status = &local_status;
	memset(status, 0, sizeof(*status));
	int rc = sync_once_collect(cfg, status);

	status->error_code = rc;
	if (rc != 0)
		(void)nb_str_copy_to(
			nb_str_from_parts(
				(nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc)),
				strnlen((nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
						    rc)),
					sizeof(status->last_error) - 1)),
			status->last_error, sizeof(status->last_error));
	return rc;
}

static void sync_file_lock_cleanup(nb_file_lock **lock)
{
	nb_file_lock_release(*lock);
	*lock = NULL;
}

int morph_sync_once(const struct morph_sync_config *cfg,
		    struct morph_sync_status *status)
{
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_4 = NB_OK;
	nb_buf file_value_5 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_5 = NB_OK;

	char path[PATH_MAX];
	int rc;
	nb_file_lock *lock NB_CLEANUP(sync_file_lock_cleanup) = NULL;

	if (status)
		memset(status, 0, sizeof(*status));
	if (!cfg || !cfg->source_dir[0])
		return NB_ERR_INVAL;
	rc = ((file_status_4 = nb_path_concat(&file_value_4, cfg->source_dir, ".morph-sync")) !=
			      NB_OK
		      ? file_status_4
	      : (file_status_4 = nb_str_copy_to(nb_buf_str(&file_value_4), path, sizeof(path))) ==
			      NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_4);
	if (rc == 0)
		rc = nb_file_mkdir(path, true, NULL);
	if (rc == 0)
		rc = ((file_status_5 = (nb_buf_clear(&file_value_5),
					nb_buf_printf(&file_value_5, "%s%s", path, "/run.lock")
						? NB_OK
						: NB_ERR_NOMEM)) != NB_OK
			      ? file_status_5
		      : (file_status_5 = nb_str_copy_to(nb_buf_str(&file_value_5), path,
							sizeof(path))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_5);
	if (rc != 0)
		return rc;
	rc = nb_file_lock_acquire(path, true, true, &lock, NULL);
	if (rc != NB_OK)
		return rc;
	rc = sync_once_locked(cfg, status);
	sync_file_lock_cleanup(&lock);
	if (status && rc != 0 && !status->error_code) {
		status->error_code = rc;
		(void)nb_str_copy_to(
			nb_str_from_parts(
				(nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc)),
				strnlen((nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
						    rc)),
					sizeof(status->last_error) - 1)),
			status->last_error, sizeof(status->last_error));
	}
	return rc;
}

static void *sync_worker_main(void *arg)
{
	struct morph_sync_worker *worker = arg;

	while (1) {
		struct morph_sync_status status;
		int interval;

		nb_mutex_lock(worker->lock);
		if (worker->stop) {
			nb_mutex_unlock(worker->lock);
			break;
		}
		worker->status.running = 1;
		nb_mutex_unlock(worker->lock);

		(void)morph_sync_once(&worker->cfg, &status);

		nb_mutex_lock(worker->lock);
		status.running = 0;
		worker->status = status;
		interval = worker->cfg.interval_seconds > 0 ?
			worker->cfg.interval_seconds : 300;
		if (!worker->stop) {
			(void)nb_cond_timedwait(worker->cond, worker->lock,
						(int64_t)interval * INT64_C(1000000000));
		}
		nb_mutex_unlock(worker->lock);
	}
	return NULL;
}

int morph_sync_start(struct morph_sync_worker *worker,
		     const struct morph_sync_config *cfg)
{
	int rc;

	if (!worker || !cfg)
		return NB_ERR_INVAL;
	if (worker->started)
		return 0;
	memset(worker, 0, sizeof(*worker));
	worker->cfg = *cfg;
	rc = ((worker->lock = nb_mutex_new(0)) ? NB_OK : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = ((worker->cond = nb_cond_new()) ? NB_OK : NB_ERR_NOMEM);
	if (rc != 0) {
		(nb_mutex_free(worker->lock), worker->lock = NULL);
		return rc;
	}
	rc = nb_thread_create(&worker->thread, sync_worker_main, worker, NULL);
	if (rc != 0) {
		(nb_cond_free(worker->cond), worker->cond = NULL);
		(nb_mutex_free(worker->lock), worker->lock = NULL);
		return rc;
	}
	worker->started = 1;
	return 0;
}

void morph_sync_stop(struct morph_sync_worker *worker)
{
	if (!worker || !worker->started)
		return;
	nb_mutex_lock(worker->lock);
	worker->stop = 1;
	nb_cond_signal(worker->cond);
	nb_mutex_unlock(worker->lock);
	nb_thread_join_take(&worker->thread, NULL);
	(nb_cond_free(worker->cond), worker->cond = NULL);
	(nb_mutex_free(worker->lock), worker->lock = NULL);
	worker->started = 0;
	worker->stop = 0;
}

int morph_sync_worker_status(struct morph_sync_worker *worker,
			     struct morph_sync_status *status)
{
	if (!worker || !status)
		return NB_ERR_INVAL;
	if (!worker->started) {
		memset(status, 0, sizeof(*status));
		return 0;
	}
	nb_mutex_lock(worker->lock);
	*status = worker->status;
	nb_mutex_unlock(worker->lock);
	return 0;
}

int morph_sync_restore_trash(const struct morph_sync_config *cfg,
			     int64_t trash_id)
{
	sqlite3 *manifest NB_CLEANUP(sync_database_cleanup) = NULL;
	sqlite3_stmt *stmt NB_CLEANUP(sync_stmt_cleanup) = NULL;
	struct sync_pair pair;
	char data_root[PATH_MAX];
	const char *rel;
	const char *trash_path;
	char rel_buf[PATH_MAX];
	char trash_buf[PATH_MAX];
	int rc;

	if (!cfg || !cfg->source_dir[0] || !cfg->sync_dir[0] || trash_id <= 0)
		return NB_ERR_INVAL;
	rc = manifest_open(&manifest, cfg->sync_dir);
	if (rc != 0)
		return rc;
	rc = sqlite3_prepare_v2(manifest,
		"SELECT path,trash_path FROM trash WHERE id=?",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		return MORPH_ERR_DB;
	}
	sqlite3_bind_int64(stmt, 1, trash_id);
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW) {
		return NB_ERR_NOT_FOUND;
	}
	rel = (const char *)sqlite3_column_text(stmt, 0);
	trash_path = (const char *)sqlite3_column_text(stmt, 1);
	(void)nb_str_copy_to(
		nb_str_from_parts((rel ? rel : ""), strnlen((rel ? rel : ""), sizeof(rel_buf) - 1)),
		rel_buf, sizeof(rel_buf));
	rel_buf[sizeof(rel_buf) - 1] = '\0';
	(void)nb_str_copy_to(
		nb_str_from_parts((trash_path ? trash_path : ""),
				  strnlen((trash_path ? trash_path : ""), sizeof(trash_buf) - 1)),
		trash_buf, sizeof(trash_buf));
	trash_buf[sizeof(trash_buf) - 1] = '\0';
	sync_stmt_cleanup(&stmt);
	if (!rel_buf[0] || !trash_buf[0]) {
		return NB_ERR_INVAL;
	}
	memset(&pair, 0, sizeof(pair));
	(void)nb_str_copy_to(
		nb_str_from_parts((rel_buf), strnlen((rel_buf), sizeof(pair.path) - 1)), pair.path,
		sizeof(pair.path));
	rc = local_path(cfg, rel_buf, pair.local.full, sizeof(pair.local.full));
	if (rc != 0)
		return rc;
	rc = sync_path_join(data_root, sizeof(data_root), cfg->sync_dir,
			    SYNC_DATA_DIR);
	if (rc != 0)
		return rc;
	rc = sync_path_join(pair.remote.full, sizeof(pair.remote.full),
			    data_root, rel_buf);
	if (rc != 0)
		return rc;
	rc = sync_copy_local_file(trash_buf, pair.local.full, rel_buf, 0);
	if (rc != 0)
		return rc;
	rc = refresh_file(&pair.local);
	if (rc != 0)
		return rc;
	(void)refresh_file(&pair.remote);
	rc = manifest_put(manifest, &pair, 0);
	if (rc != 0)
		return rc;
	rc = manifest_clear_tombstone(manifest, rel_buf);
	if (rc != 0)
		return rc;
	rc = manifest_delete_trash(manifest, trash_id);

	return rc;
}
