#include "sync/sync.h"
#include "util/array.h"
#include "util/error.h"
#include "util/file.h"
#include "util/log.h"
#include "blake3.h"
#include <uv.h>
#include <sqlite3.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static int ensure_parent_dir(const char *path)
{
	char dir[PATH_MAX];
	char *slash;

	if (!path)
		MORPH_RETURN(-EINVAL);
	strncpy(dir, path, sizeof(dir) - 1);
	dir[sizeof(dir) - 1] = '\0';
	slash = strrchr(dir, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	if (!dir[0])
		return 0;
	return file_ensure_dir(dir);
}

static int sync_path_join(char *dst, size_t size, const char *a,
			  const char *b)
{
	int rc;

	rc = file_path_join(dst, size, a, b);
	if (rc != 0)
		return rc;
	return 0;
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

static int hash_file(const char *path, char out[MORPH_SYNC_HASH_LEN])
{
	unsigned char buf[BUFSIZ];
	unsigned char hash[BLAKE3_OUT_LEN];
	blake3_hasher hasher;
	FILE *f;
	size_t n;

	if (!path || !out)
		MORPH_RETURN(-EINVAL);
	f = fopen(path, "rb");
	if (!f)
		MORPH_RETURN_ERRNO();
	blake3_hasher_init(&hasher);
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		blake3_hasher_update(&hasher, buf, n);
	}
	if (ferror(f)) {
		fclose(f);
		MORPH_RETURN(-EIO);
	}
	fclose(f);
	blake3_hasher_finalize(&hasher, hash, BLAKE3_OUT_LEN);
	hash_to_hex(hash, out);
	return 0;
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

static struct sync_pair *find_pair(morph_array_t *pairs, const char *rel)
{
	struct sync_pair *items;

	items = pairs->elts;
	for (size_t i = 0; i < pairs->nelts; i++) {
		if (strcmp(items[i].path, rel) == 0)
			return &items[i];
	}
	return NULL;
}

static int add_file(morph_array_t *pairs, const char *root, const char *rel,
		    int remote_side)
{
	struct sync_pair *pair;
	struct sync_file *file;
	uv_fs_t req;
	int rc;

	if (should_skip_rel(rel))
		return 0;
	pair = find_pair(pairs, rel);
	if (!pair) {
		pair = morph_array_push(pairs);
		if (!pair)
			MORPH_RETURN(-ENOMEM);
		memset(pair, 0, sizeof(*pair));
		strncpy(pair->path, rel, sizeof(pair->path) - 1);
	}
	file = remote_side ? &pair->remote : &pair->local;
	strncpy(file->path, rel, sizeof(file->path) - 1);
	rc = sync_path_join(file->full, sizeof(file->full), root, rel);
	if (rc != 0)
		return rc;
	rc = uv_fs_stat(NULL, &req, file->full, NULL);
	if (rc < 0) {
		uv_fs_req_cleanup(&req);
		MORPH_RETURN(-ENOENT);
	}
	file->exists = 1;
	file->size = (int64_t)req.statbuf.st_size;
	file->mtime = (int64_t)req.statbuf.st_mtim.tv_sec;
	uv_fs_req_cleanup(&req);
	return hash_file(file->full, file->hash);
}

static int refresh_file(struct sync_file *file)
{
	uv_fs_t req;
	int rc;

	if (!file || !file->full[0])
		MORPH_RETURN(-EINVAL);
	rc = uv_fs_stat(NULL, &req, file->full, NULL);
	if (rc < 0) {
		uv_fs_req_cleanup(&req);
		file->exists = 0;
		file->hash[0] = '\0';
		file->size = 0;
		file->mtime = 0;
		return 0;
	}
	file->exists = 1;
	file->size = (int64_t)req.statbuf.st_size;
	file->mtime = (int64_t)req.statbuf.st_mtim.tv_sec;
	uv_fs_req_cleanup(&req);
	return hash_file(file->full, file->hash);
}

static int scan_dir(morph_array_t *pairs, const char *root, const char *rel_dir,
		    int remote_side)
{
	char full[PATH_MAX];
	uv_fs_t req;
	uv_dirent_t ent;
	int rc;

	rc = sync_path_join(full, sizeof(full), root, rel_dir);
	if (rc != 0)
		return rc;
	rc = uv_fs_scandir(NULL, &req, full, 0, NULL);
	if (rc == UV_ENOENT) {
		uv_fs_req_cleanup(&req);
		return 0;
	}
	if (rc < 0) {
		uv_fs_req_cleanup(&req);
		MORPH_RETURN(-EIO);
	}
	while (uv_fs_scandir_next(&req, &ent) != UV_EOF) {
		char child_rel[PATH_MAX];
		if (rel_dir[0]) {
			rc = file_path_join(child_rel, sizeof(child_rel),
					    rel_dir, ent.name);
		} else {
			strncpy(child_rel, ent.name, sizeof(child_rel) - 1);
			child_rel[sizeof(child_rel) - 1] = '\0';
			rc = 0;
		}
		if (rc != 0 || should_skip_rel(child_rel))
			continue;
		if (ent.type == UV_DIRENT_DIR) {
			rc = scan_dir(pairs, root, child_rel, remote_side);
			if (rc != 0)
				break;
		} else if (ent.type == UV_DIRENT_FILE ||
			   ent.type == UV_DIRENT_UNKNOWN) {
			rc = add_file(pairs, root, child_rel, remote_side);
			if (rc != 0)
				break;
		}
	}
	uv_fs_req_cleanup(&req);
	return rc;
}

static int scan_include(morph_array_t *pairs, const char *root,
			const char *include, int remote_side)
{
	char full[PATH_MAX];
	uv_fs_t req;
	int rc;

	rc = sync_path_join(full, sizeof(full), root, include);
	if (rc != 0)
		return rc;
	rc = uv_fs_stat(NULL, &req, full, NULL);
	if (rc == UV_ENOENT) {
		uv_fs_req_cleanup(&req);
		return 0;
	}
	if (rc < 0) {
		uv_fs_req_cleanup(&req);
		return 0;
	}
	if (S_ISDIR(req.statbuf.st_mode)) {
		uv_fs_req_cleanup(&req);
		return scan_dir(pairs, root, include, remote_side);
	}
	uv_fs_req_cleanup(&req);
	return add_file(pairs, root, include, remote_side);
}

static int scan_side(morph_array_t *pairs, const char *root,
		     const struct morph_sync_config *cfg, int remote_side)
{
	if (!root || !root[0])
		MORPH_RETURN(-EINVAL);
	for (int i = 0; i < cfg->include_count; i++) {
		int rc = scan_include(pairs, root, cfg->include[i],
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
	rc = file_ensure_dir(meta);
	if (rc != 0)
		return rc;
	rc = sync_path_join(path, sizeof(path), meta, SYNC_MANIFEST);
	if (rc != 0)
		return rc;
	if (sqlite3_open_v2(path, out, SQLITE_OPEN_READWRITE |
	    SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
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
		"created_at INTEGER NOT NULL);"
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
		MORPH_RETURN(MORPH_ERR_DB);
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
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		const char *lh = (const char *)sqlite3_column_text(stmt, 0);
		const char *rh = (const char *)sqlite3_column_text(stmt, 1);
		out->found = 1;
		if (lh)
			strncpy(out->local_hash, lh,
				sizeof(out->local_hash) - 1);
		if (rh)
			strncpy(out->remote_hash, rh,
				sizeof(out->remote_hash) - 1);
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
		MORPH_RETURN(MORPH_ERR_DB);
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
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_text(stmt, 1, pair->path, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, pair->local.hash, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, pair->remote.hash, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 4, (int64_t)time(NULL));
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
		MORPH_RETURN(MORPH_ERR_DB);
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
		MORPH_RETURN(MORPH_ERR_DB);
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
		MORPH_RETURN(MORPH_ERR_DB);
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
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int copy_sqlite_backup(const char *src, const char *dst)
{
	sqlite3 *from = NULL;
	sqlite3 *to = NULL;
	sqlite3_backup *backup;
	int rc;

	rc = sqlite3_open_v2(src, &from, SQLITE_OPEN_READONLY, NULL);
	if (rc != SQLITE_OK)
		goto fail;
	rc = sqlite3_open_v2(dst, &to, SQLITE_OPEN_READWRITE |
			     SQLITE_OPEN_CREATE, NULL);
	if (rc != SQLITE_OK)
		goto fail;
	backup = sqlite3_backup_init(to, "main", from, "main");
	if (!backup)
		goto fail;
	(void)sqlite3_backup_step(backup, -1);
	rc = sqlite3_backup_finish(backup);
	sqlite3_close(from);
	sqlite3_close(to);
	return rc == SQLITE_OK ? 0 : MORPH_ERR_DB;

fail:
	if (from)
		sqlite3_close(from);
	if (to)
		sqlite3_close(to);
	return -EINVAL;
}

static int copy_file_plain(const char *src, const char *dst)
{
	uv_fs_t req;
	int rc;

	rc = uv_fs_copyfile(NULL, &req, src, dst, 0, NULL);
	uv_fs_req_cleanup(&req);
	if (rc < 0)
		MORPH_RETURN(-EIO);
	return 0;
}

static int copy_atomic(const struct morph_sync_config *cfg, const char *src,
		       const char *dst, const char *rel, int sqlite_source)
{
	char meta[PATH_MAX];
	char staging_dir[PATH_MAX];
	char staging[PATH_MAX];
	uv_fs_t req;
	int rc;

	rc = sync_path_join(meta, sizeof(meta), cfg->sync_dir, SYNC_META_DIR);
	if (rc != 0)
		return rc;
	rc = sync_path_join(staging_dir, sizeof(staging_dir), meta,
			    SYNC_STAGING);
	if (rc != 0)
		return rc;
	rc = file_ensure_dir(staging_dir);
	if (rc != 0)
		return rc;
	snprintf(staging, sizeof(staging), "%s/%lld-%u.tmp", staging_dir,
		 (long long)time(NULL), (unsigned)getpid());
	if (sqlite_source && path_has_suffix(rel, ".db")) {
		rc = copy_sqlite_backup(src, staging);
		if (rc != 0)
			rc = copy_file_plain(src, staging);
	} else {
		rc = copy_file_plain(src, staging);
	}
	if (rc != 0)
		return rc;
	rc = ensure_parent_dir(dst);
	if (rc != 0)
		return rc;
	rc = uv_fs_rename(NULL, &req, staging, dst, NULL);
	uv_fs_req_cleanup(&req);
	if (rc < 0)
		MORPH_RETURN(-EIO);
	return 0;
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
	char trash[PATH_MAX];
	uv_fs_t req;
	int rc;

	if (!file_exists(path))
		return 0;
	rc = sync_path_join(meta, sizeof(meta), cfg->sync_dir, SYNC_META_DIR);
	if (rc != 0)
		return rc;
	rc = sync_path_join(trash_dir, sizeof(trash_dir), meta, SYNC_TRASH);
	if (rc != 0)
		return rc;
	rc = file_ensure_dir(trash_dir);
	if (rc != 0)
		return rc;
	sanitize_rel(safe, sizeof(safe), rel);
	snprintf(trash, sizeof(trash), "%s/%lld-%u-%s", trash_dir,
		 (long long)time(NULL), (unsigned)getpid(), safe);
	rc = copy_file_plain(path, trash);
	if (rc != 0)
		return rc;
	rc = uv_fs_unlink(NULL, &req, path, NULL);
	uv_fs_req_cleanup(&req);
	if (rc < 0)
		MORPH_RETURN(-EIO);
	rc = manifest_trash(db, rel, trash);
	if (rc != 0)
		return rc;
	return manifest_tombstone(db, rel);
}

static int create_conflict(sqlite3 *db, const struct morph_sync_config *cfg,
			   const struct sync_pair *pair)
{
	char local_conflict[PATH_MAX];
	char remote_conflict[PATH_MAX];
	char suffix[128];
	int rc;

	snprintf(suffix, sizeof(suffix), ".conflict.%lld.%u",
		 (long long)time(NULL), (unsigned)getpid());
	snprintf(local_conflict, sizeof(local_conflict), "%s%s",
		 pair->path, suffix);
	snprintf(remote_conflict, sizeof(remote_conflict), "%s%s",
		 pair->path, suffix);
	if (pair->remote.exists) {
		char dst[PATH_MAX];
		rc = sync_path_join(dst, sizeof(dst), cfg->source_dir,
				    local_conflict);
		if (rc != 0)
			return rc;
		rc = copy_atomic(cfg, pair->remote.full, dst, pair->path, 0);
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
		rc = sync_path_join(dst, sizeof(dst), data_root,
				    remote_conflict);
		if (rc != 0)
			return rc;
		rc = copy_atomic(cfg, pair->local.full, dst, pair->path, 1);
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
		rc = sync_path_join(pair->local.full, sizeof(pair->local.full),
				    cfg->source_dir, pair->path);
		if (rc != 0)
			return rc;
	}
	if (!pair->remote.full[0]) {
		rc = sync_path_join(pair->remote.full, sizeof(pair->remote.full),
				    data_root, pair->path);
		if (rc != 0)
			return rc;
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
				rc = copy_atomic(cfg, pair->local.full,
						 pair->remote.full,
						 pair->path, 1);
				if (rc != 0)
					return rc;
				status->copied++;
				rc = refresh_file(&pair->remote);
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
			rc = copy_atomic(cfg, pair->local.full,
					 pair->remote.full, pair->path, 1);
			if (rc != 0)
				return rc;
			status->copied++;
			rc = refresh_file(&pair->remote);
			if (rc != 0)
				return rc;
			return manifest_put(db, pair, 0);
		}
		if (remote_changed) {
			rc = copy_atomic(cfg, pair->remote.full,
					 pair->local.full, pair->path, 0);
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
		rc = copy_atomic(cfg, pair->local.full, pair->remote.full,
				 pair->path, 1);
		if (rc != 0)
			return rc;
		status->copied++;
		rc = refresh_file(&pair->remote);
		if (rc != 0)
			return rc;
		return manifest_put(db, pair, 0);
	}

	if (!pair->local.exists && pair->remote.exists) {
		if (old.local_exists && local_changed && !remote_changed) {
			rc = move_to_trash(db, cfg, pair->remote.full, pair->path);
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
		rc = copy_atomic(cfg, pair->remote.full, pair->local.full,
				 pair->path, 0);
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

	rc = file_ensure_dir(cfg->sync_dir);
	if (rc != 0)
		return rc;
	rc = sync_path_join(data, sizeof(data), cfg->sync_dir, SYNC_DATA_DIR);
	if (rc != 0)
		return rc;
	rc = file_ensure_dir(data);
	if (rc != 0)
		return rc;
	rc = sync_path_join(meta, sizeof(meta), cfg->sync_dir, SYNC_META_DIR);
	if (rc != 0)
		return rc;
	rc = file_ensure_dir(meta);
	if (rc != 0)
		return rc;
	rc = sync_path_join(staging, sizeof(staging), meta, SYNC_STAGING);
	if (rc != 0)
		return rc;
	rc = file_ensure_dir(staging);
	if (rc != 0)
		return rc;
	rc = sync_path_join(trash, sizeof(trash), meta, SYNC_TRASH);
	if (rc != 0)
		return rc;
	return file_ensure_dir(trash);
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
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, cutoff);
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
		const char *path = (const char *)sqlite3_column_text(stmt, 1);
		if (path && file_exists(path)) {
			uv_fs_t req;
			(void)uv_fs_unlink(NULL, &req, path, NULL);
			uv_fs_req_cleanup(&req);
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

int morph_sync_once(const struct morph_sync_config *cfg,
		    struct morph_sync_status *status)
{
	struct morph_sync_status local_status;
	morph_array_t pairs;
	sqlite3 *manifest = NULL;
	char data_root[PATH_MAX];
	int rc;

	if (!cfg || !cfg->source_dir[0] || !cfg->sync_dir[0])
		MORPH_RETURN(-EINVAL);
	if (!status)
		status = &local_status;
	memset(status, 0, sizeof(*status));
	rc = prepare_dirs(cfg);
	if (rc != 0)
		goto out_no_array;
	rc = manifest_open(&manifest, cfg->sync_dir);
	if (rc != 0)
		goto out_no_array;
	rc = morph_array_init(&pairs, 64, sizeof(struct sync_pair));
	if (rc != 0)
		goto out_no_array;
	rc = sync_path_join(data_root, sizeof(data_root), cfg->sync_dir,
			    SYNC_DATA_DIR);
	if (rc != 0)
		goto out;
	rc = scan_side(&pairs, cfg->source_dir, cfg, 0);
	if (rc != 0)
		goto out;
	rc = scan_side(&pairs, data_root, cfg, 1);
	if (rc != 0)
		goto out;
	for (size_t i = 0; i < pairs.nelts; i++) {
		struct sync_pair *pair =
			&((struct sync_pair *)pairs.elts)[i];
		rc = sync_pair(manifest, cfg, pair, status);
		if (rc != 0)
			goto out;
	}
	rc = cleanup_trash(manifest, cfg);
	if (rc != 0)
		goto out;
	status->last_run_at = (int64_t)time(NULL);
	status->conflicts = count_table(manifest, "conflicts");
	status->recycled = count_table(manifest, "trash");

out:
	morph_array_cleanup(&pairs);
out_no_array:
	if (manifest)
		sqlite3_close(manifest);
	status->error_code = rc;
	if (rc != 0) {
		strncpy(status->last_error, morph_strerror(rc),
			sizeof(status->last_error) - 1);
	}
	return rc;
}

static void *sync_worker_main(void *arg)
{
	struct morph_sync_worker *worker = arg;

	while (1) {
		struct morph_sync_status status;
		int interval;

		pthread_mutex_lock(&worker->lock);
		if (worker->stop) {
			pthread_mutex_unlock(&worker->lock);
			break;
		}
		worker->status.running = 1;
		pthread_mutex_unlock(&worker->lock);

		(void)morph_sync_once(&worker->cfg, &status);

		pthread_mutex_lock(&worker->lock);
		status.running = 0;
		worker->status = status;
		interval = worker->cfg.interval_seconds > 0 ?
			worker->cfg.interval_seconds : 300;
		if (!worker->stop) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += interval;
			(void)pthread_cond_timedwait(&worker->cond,
						     &worker->lock, &ts);
		}
		pthread_mutex_unlock(&worker->lock);
	}
	return NULL;
}

int morph_sync_start(struct morph_sync_worker *worker,
		     const struct morph_sync_config *cfg)
{
	int rc;

	if (!worker || !cfg)
		MORPH_RETURN(-EINVAL);
	if (worker->started)
		return 0;
	memset(worker, 0, sizeof(*worker));
	worker->cfg = *cfg;
	rc = pthread_mutex_init(&worker->lock, NULL);
	if (rc != 0)
		MORPH_RETURN(-rc);
	rc = pthread_cond_init(&worker->cond, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&worker->lock);
		MORPH_RETURN(-rc);
	}
	rc = pthread_create(&worker->thread, NULL, sync_worker_main, worker);
	if (rc != 0) {
		pthread_cond_destroy(&worker->cond);
		pthread_mutex_destroy(&worker->lock);
		MORPH_RETURN(-rc);
	}
	worker->started = 1;
	return 0;
}

void morph_sync_stop(struct morph_sync_worker *worker)
{
	if (!worker || !worker->started)
		return;
	pthread_mutex_lock(&worker->lock);
	worker->stop = 1;
	pthread_cond_signal(&worker->cond);
	pthread_mutex_unlock(&worker->lock);
	pthread_join(worker->thread, NULL);
	pthread_cond_destroy(&worker->cond);
	pthread_mutex_destroy(&worker->lock);
	worker->started = 0;
	worker->stop = 0;
}

int morph_sync_worker_status(struct morph_sync_worker *worker,
			     struct morph_sync_status *status)
{
	if (!worker || !status)
		MORPH_RETURN(-EINVAL);
	if (!worker->started) {
		memset(status, 0, sizeof(*status));
		return 0;
	}
	pthread_mutex_lock(&worker->lock);
	*status = worker->status;
	pthread_mutex_unlock(&worker->lock);
	return 0;
}

int morph_sync_restore_trash(const struct morph_sync_config *cfg,
			     int64_t trash_id)
{
	sqlite3 *manifest = NULL;
	sqlite3_stmt *stmt = NULL;
	struct sync_pair pair;
	char data_root[PATH_MAX];
	const char *rel;
	const char *trash_path;
	char rel_buf[PATH_MAX];
	char trash_buf[PATH_MAX];
	int rc;

	if (!cfg || !cfg->source_dir[0] || !cfg->sync_dir[0] || trash_id <= 0)
		MORPH_RETURN(-EINVAL);
	rc = manifest_open(&manifest, cfg->sync_dir);
	if (rc != 0)
		return rc;
	rc = sqlite3_prepare_v2(manifest,
		"SELECT path,trash_path FROM trash WHERE id=?",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		sqlite3_close(manifest);
		MORPH_RETURN(MORPH_ERR_DB);
	}
	sqlite3_bind_int64(stmt, 1, trash_id);
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW) {
		sqlite3_finalize(stmt);
		sqlite3_close(manifest);
		MORPH_RETURN(-ENOENT);
	}
	rel = (const char *)sqlite3_column_text(stmt, 0);
	trash_path = (const char *)sqlite3_column_text(stmt, 1);
	strncpy(rel_buf, rel ? rel : "", sizeof(rel_buf) - 1);
	rel_buf[sizeof(rel_buf) - 1] = '\0';
	strncpy(trash_buf, trash_path ? trash_path : "", sizeof(trash_buf) - 1);
	trash_buf[sizeof(trash_buf) - 1] = '\0';
	sqlite3_finalize(stmt);
	if (!rel_buf[0] || !trash_buf[0]) {
		sqlite3_close(manifest);
		MORPH_RETURN(-EINVAL);
	}
	memset(&pair, 0, sizeof(pair));
	strncpy(pair.path, rel_buf, sizeof(pair.path) - 1);
	rc = sync_path_join(pair.local.full, sizeof(pair.local.full),
			    cfg->source_dir, rel_buf);
	if (rc != 0)
		goto out;
	rc = sync_path_join(data_root, sizeof(data_root), cfg->sync_dir,
			    SYNC_DATA_DIR);
	if (rc != 0)
		goto out;
	rc = sync_path_join(pair.remote.full, sizeof(pair.remote.full),
			    data_root, rel_buf);
	if (rc != 0)
		goto out;
	rc = copy_atomic(cfg, trash_buf, pair.local.full, rel_buf, 0);
	if (rc != 0)
		goto out;
	rc = refresh_file(&pair.local);
	if (rc != 0)
		goto out;
	(void)refresh_file(&pair.remote);
	rc = manifest_put(manifest, &pair, 0);
	if (rc != 0)
		goto out;
	rc = manifest_clear_tombstone(manifest, rel_buf);
	if (rc != 0)
		goto out;
	rc = manifest_delete_trash(manifest, trash_id);

out:
	sqlite3_close(manifest);
	return rc;
}
