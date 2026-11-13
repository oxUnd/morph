#include "checkpoint_store.h"

#include "nb/vec.h"
#include "nb/cleanup.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/random.h"
#include "nb/encoding.h"
#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHECKPOINT_PLAN_MAX_FILES 4096U

struct checkpoint_lock {
	sqlite3_mutex *mutex;
};

struct checkpoint_state {
	unsigned char *bytes;
	size_t size;
	mode_t mode;
	int exists;
};

struct checkpoint_file {
	char *path;
	struct checkpoint_state before;
	struct checkpoint_state after;
	struct checkpoint_state current;
	int sealed;
	int pending;
	int applied;
	int compensated;
	int conflict;
	char backup[80];
	char compensation_backup[80];
	char staging[80];
	char compensation_staging[80];
	int staging_owned;
	int compensation_staging_owned;
};

struct checkpoint_info {
	int64_t id;
	int64_t session_id;
	int64_t created_at;
	char *root;
	char *turn_id;
	char *prompt;
	char *reason;
	int code_active;
	int conversation_active;
};

struct checkpoint_plan {
	struct checkpoint_info info;
	nb_vec files;
	size_t bytes;
};

/* Delete in this order; restore in reverse order to satisfy foreign keys. */
struct checkpoint_table {
	const char *name;
	const char *filter;
};

static void checkpoint_fd_cleanup(int *fd)
{
	if (*fd >= 0)
		close(*fd);
	*fd = -1;
}

static void checkpoint_stmt_cleanup(sqlite3_stmt **stmt)
{
	sqlite3_finalize(*stmt);
	*stmt = NULL;
}

static void checkpoint_lock_cleanup(struct checkpoint_lock *lock)
{
	if (lock->mutex)
		sqlite3_mutex_leave(lock->mutex);
	lock->mutex = NULL;
}

static void checkpoint_state_cleanup(struct checkpoint_state *state)
{
	free(state->bytes);
	memset(state, 0, sizeof(*state));
}

static void checkpoint_info_cleanup(struct checkpoint_info *info)
{
	free(info->root);
	free(info->turn_id);
	free(info->prompt);
	free(info->reason);
	memset(info, 0, sizeof(*info));
}

static void checkpoint_plan_cleanup(struct checkpoint_plan *plan)
{
	struct checkpoint_file *file;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&plan->files) &&
			     (file = (struct checkpoint_file *)nb_vec_at(&plan->files, _i_));
	     _i_++) {
		free(file->path);
		checkpoint_state_cleanup(&file->before);
		checkpoint_state_cleanup(&file->after);
		checkpoint_state_cleanup(&file->current);
	}
	nb_vec_cleanup(&plan->files);
	checkpoint_info_cleanup(&plan->info);
}

static int checkpoint_prepare(struct db *db, const char *sql, sqlite3_stmt **out)
{
	return sqlite3_prepare_v2(db->handle, sql, -1, out, NULL) == SQLITE_OK ? 0 : MORPH_ERR_DB;
}

static int checkpoint_done(sqlite3_stmt *stmt)
{
	return sqlite3_step(stmt) == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int checkpoint_table_exists(struct db *db, const char *name)
{
	sqlite3_stmt *stmt NB_CLEANUP(checkpoint_stmt_cleanup) = NULL;
	int rc = checkpoint_prepare(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
				    &stmt);
	if (rc != 0)
		return rc;
	sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
	rc = sqlite3_step(stmt);
	return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int checkpoint_tables(struct checkpoint_table *tables, size_t capacity)
{
	const struct checkpoint_table source[] = {
		{"message_attachments",
		 "message_id IN (SELECT id FROM messages WHERE session_id=?2)"},
		{"history_summary_sources",
		 "summary_item_id IN (SELECT id FROM model_history_items WHERE session_id=?2)"},
		{"history_compactions", "session_id=?2"},
		{"history_compaction_attempts", "session_id=?2"},
		{"model_request_snapshots", "session_id=?2"},
		{"react_traces", "session_id=?2"},
		{"session_plans", "session_id=?2"},
		{"session_skill_activations", "session_id=?2"},
		{"model_history_items", "session_id=?2"},
		{"messages", "session_id=?2"},
	};
	if (capacity < sizeof(source) / sizeof(source[0]))
		return NB_ERR_RANGE;
	memcpy(tables, source, sizeof(source));
	return (int)(sizeof(source) / sizeof(source[0]));
}

static int checkpoint_schema(struct db *db)
{
	const char *sql =
		"CREATE TABLE IF NOT EXISTS code_checkpoints("
		"id INTEGER PRIMARY KEY AUTOINCREMENT,session_id INTEGER NOT NULL REFERENCES "
		"sessions(id) ON DELETE CASCADE,"
		"workspace_root TEXT NOT NULL,turn_id TEXT NOT NULL,prompt TEXT NOT "
		"NULL,created_at INTEGER NOT NULL,"
		"code_active INTEGER NOT NULL DEFAULT 1,conversation_active INTEGER NOT NULL "
		"DEFAULT 1,reason TEXT NOT NULL DEFAULT '');"
		"CREATE INDEX IF NOT EXISTS code_checkpoints_session ON "
		"code_checkpoints(session_id,id);"
		"CREATE TABLE IF NOT EXISTS code_checkpoint_files("
		"checkpoint_id INTEGER NOT NULL REFERENCES code_checkpoints(id) ON DELETE "
		"CASCADE,path TEXT NOT NULL,"
		"before_exists INTEGER NOT NULL,before_mode INTEGER NOT NULL,before_data BLOB,"
		"after_exists INTEGER NOT NULL,after_mode INTEGER NOT NULL,after_data BLOB,"
		"sealed INTEGER NOT NULL DEFAULT 0,pending_token TEXT,PRIMARY "
		"KEY(checkpoint_id,path));"
		"CREATE TEMP TABLE IF NOT EXISTS checkpoint_connection_token(token TEXT NOT NULL);"
		"INSERT INTO checkpoint_connection_token SELECT lower(hex(randomblob(16))) "
		"WHERE NOT EXISTS(SELECT 1 FROM checkpoint_connection_token);";
	int rc = db_exec(db, sql);
	struct checkpoint_table tables[10];
	int count = checkpoint_tables(tables, sizeof(tables) / sizeof(tables[0]));
	for (int i = 0; rc == 0 && i < count; i++) {
		int present = checkpoint_table_exists(db, tables[i].name);
		if (present < 0)
			return present;
		if (!present)
			continue;
		nb_buf query NB_CLEANUP(nb_buf_cleanup) = {0};
		rc = (nb_buf_init(&query), nb_buf_reserve(&query, 256) ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_printf(
				      &query,
				      "CREATE TABLE IF NOT EXISTS checkpoint_rows_%s AS "
				      "SELECT CAST(0 AS INTEGER) AS checkpoint_id,t.* FROM %s t "
				      "WHERE 0;"
				      "CREATE INDEX IF NOT EXISTS checkpoint_rows_%s_owner ON "
				      "checkpoint_rows_%s(checkpoint_id);"
				      "CREATE TRIGGER IF NOT EXISTS checkpoint_rows_%s_delete "
				      "AFTER "
				      "DELETE ON code_checkpoints "
				      "BEGIN DELETE FROM checkpoint_rows_%s WHERE "
				      "checkpoint_id=OLD.id; "
				      "END;",
				      tables[i].name, tables[i].name, tables[i].name,
				      tables[i].name, tables[i].name, tables[i].name)
				      ? 0
				      : NB_ERR_NOMEM);
		if (rc == 0)
			rc = db_exec(db, nb_buf_cstr(&query));
	}
	if (rc == 0)
		rc = db_exec(
			db,
			"CREATE TEMP TABLE IF NOT EXISTS checkpoint_schema_ready(ready INTEGER)");
	return rc;
}

static int checkpoint_enter(struct db *db, struct checkpoint_lock *lock)
{
	if (!db || !db->handle)
		return NB_ERR_INVAL;
	lock->mutex = sqlite3_db_mutex(db->handle);
	if (lock->mutex)
		sqlite3_mutex_enter(lock->mutex);
	if (!sqlite3_get_autocommit(db->handle))
		return NB_ERR_BUSY;
	sqlite3_stmt *stmt NB_CLEANUP(checkpoint_stmt_cleanup) = NULL;
	int rc = checkpoint_prepare(db,
				    "SELECT 1 FROM sqlite_temp_master WHERE type='table' AND "
				    "name='checkpoint_schema_ready'",
				    &stmt);
	if (rc != 0)
		return rc;
	int step = sqlite3_step(stmt);
	if (step == SQLITE_ROW)
		return 0;
	if (step != SQLITE_DONE)
		return MORPH_ERR_DB;
	return checkpoint_schema(db);
}

static int checkpoint_transaction_finish(struct db *db, int rc)
{
	if (rc != 0) {
		(void)db_exec(db, "ROLLBACK TO morph_checkpoint; RELEASE morph_checkpoint;");
		return rc;
	}
	rc = db_exec(db, "RELEASE morph_checkpoint;");
	if (rc != 0)
		(void)db_exec(db, "ROLLBACK TO morph_checkpoint; RELEASE morph_checkpoint;");
	return rc;
}

static int checkpoint_info_read(struct db *db, int64_t session_id, int64_t id,
				struct checkpoint_info *out)
{
	sqlite3_stmt *stmt NB_CLEANUP(checkpoint_stmt_cleanup) = NULL;
	int rc = checkpoint_prepare(db,
				    "SELECT session_id,workspace_root,turn_id,prompt,created_at,"
				    "code_active,conversation_active,reason FROM code_checkpoints "
				    "WHERE id=? AND (?=0 OR session_id=?) "
				    "AND created_at>=strftime('%s','now')-2592000",
				    &stmt);
	if (rc != 0)
		return rc;
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, session_id);
	sqlite3_bind_int64(stmt, 3, session_id);
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW)
		return rc == SQLITE_DONE ? NB_ERR_NOT_FOUND : MORPH_ERR_DB;
	out->id = id;
	out->session_id = sqlite3_column_int64(stmt, 0);
	out->root = strdup((const char *)sqlite3_column_text(stmt, 1));
	out->turn_id = strdup((const char *)sqlite3_column_text(stmt, 2));
	out->prompt = strdup((const char *)sqlite3_column_text(stmt, 3));
	out->created_at = sqlite3_column_int64(stmt, 4);
	out->code_active = sqlite3_column_int(stmt, 5);
	out->conversation_active = sqlite3_column_int(stmt, 6);
	out->reason = strdup((const char *)sqlite3_column_text(stmt, 7));
	return out->root && out->turn_id && out->prompt && out->reason ? 0 : NB_ERR_NOMEM;
}

static int checkpoint_state_read_sql(sqlite3_stmt *stmt, int offset, struct checkpoint_state *out)
{
	out->exists = sqlite3_column_int(stmt, offset);
	out->mode = (mode_t)sqlite3_column_int(stmt, offset + 1);
	int size = sqlite3_column_bytes(stmt, offset + 2);
	if (size < 0 || (unsigned int)size > CHECKPOINT_FILE_MAX_BYTES)
		return NB_ERR_TOO_BIG;
	out->size = (size_t)size;
	if (size) {
		out->bytes = malloc(out->size);
		if (!out->bytes)
			return NB_ERR_NOMEM;
		memcpy(out->bytes, sqlite3_column_blob(stmt, offset + 2), out->size);
	}
	return 0;
}

static int checkpoint_state_bind(sqlite3_stmt *stmt, int index,
				 const struct checkpoint_state *state)
{
	if (state->size > INT_MAX)
		return NB_ERR_TOO_BIG;
	int rc = sqlite3_bind_int(stmt, index, state->exists);
	if (rc == SQLITE_OK)
		rc = sqlite3_bind_int(stmt, index + 1, (int)state->mode);
	if (rc == SQLITE_OK)
		rc = state->size     ? sqlite3_bind_blob(stmt, index + 2, state->bytes,
							 (int)state->size, SQLITE_TRANSIENT)
		     : state->exists ? sqlite3_bind_zeroblob(stmt, index + 2, 0)
				     : sqlite3_bind_null(stmt, index + 2);
	return rc == SQLITE_OK ? 0 : MORPH_ERR_DB;
}

static int checkpoint_state_equal(const struct checkpoint_state *a,
				  const struct checkpoint_state *b)
{
	return a->exists == b->exists &&
	       (!a->exists || (a->mode == b->mode && a->size == b->size &&
			       (!a->size || !memcmp(a->bytes, b->bytes, a->size))));
}

/* Paths are normalized lexically before walking every directory through an fd. */
static int checkpoint_relative(const char *root, const char *path, char **out)
{
	if (!root || !path || !*path)
		return NB_ERR_INVAL;
	if (path[0] == '/') {
		/* Resolve only the workspace prefix. Resolving the whole candidate
		 * would hide symlinks below the workspace from the fd walk. */
		const char *relative = !strcmp(root, "/") ? path + 1 : NULL;
		size_t path_length = strlen(path);
		for (size_t length = 1; !relative && length <= path_length; length++) {
			if (path[length] && path[length] != '/')
				continue;
			char *prefix NB_CLEANUP(nb_charp_cleanup) = strndup(path, length);
			char *resolved NB_CLEANUP(nb_charp_cleanup) =
				prefix ? realpath(prefix, NULL) : NULL;
			if (!prefix)
				return NB_ERR_NOMEM;
			if (resolved && !strcmp(resolved, root)) {
				relative = path + length;
				break;
			}
		}
		if (!relative)
			return NB_ERR_PERMISSION;
		path = relative;
	}
	char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(path);
	nb_buf normalized NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!copy)
		return NB_ERR_NOMEM;
	int rc = (nb_buf_init(&normalized),
		  nb_buf_reserve(&normalized, strlen(path) + 1) ? 0 : NB_ERR_NOMEM);
	char *save = NULL;
	for (char *part = strtok_r(copy, "/", &save); rc == 0 && part;
	     part = strtok_r(NULL, "/", &save)) {
		if (!strcmp(part, "."))
			continue;
		if (!strcmp(part, ".."))
			return NB_ERR_PERMISSION;
		if (normalized.len)
			rc = (nb_buf_append_char(&normalized, '/') ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_cstr(&normalized, part) ? 0 : NB_ERR_NOMEM);
	}
	if (rc != 0)
		return rc;
	if (!normalized.len || normalized.len >= PATH_MAX)
		return normalized.len ? NB_ERR_NAME_TOO_LONG : NB_ERR_INVAL;
	*out = (nb_buf_ok(&normalized) ? strdup(nb_buf_cstr(&normalized)) : NULL);
	return *out ? 0 : NB_ERR_NOMEM;
}

static int checkpoint_walk(const char *root, const char *relative, int *parent, char **leaf)
{
	char *full NB_CLEANUP(nb_charp_cleanup) = NULL;
	nb_buf path NB_CLEANUP(nb_buf_cleanup) = {0};
	int fd NB_CLEANUP(checkpoint_fd_cleanup) = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return nb_status_from_errno(errno, NULL);
	int rc = (nb_buf_init(&path),
		  nb_buf_reserve(&path, strlen(root) + strlen(relative) + 2) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_printf(&path, "%s/%s", root, relative) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	full = (nb_buf_ok(&path) ? strdup(nb_buf_cstr(&path)) : NULL);
	if (!full)
		return NB_ERR_NOMEM;
	char *save = NULL;
	char *part = strtok_r(full, "/", &save);
	while (part) {
		char *next = strtok_r(NULL, "/", &save);
		if (!next) {
			*leaf = strdup(part);
			if (!*leaf)
				return NB_ERR_NOMEM;
			*parent = fd;
			fd = -1;
			return 0;
		}
		int child = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (child < 0)
			return nb_status_from_errno(errno, NULL);
		close(fd);
		fd = child;
		part = next;
	}
	return NB_ERR_INVAL;
}

static int checkpoint_same_stat(const struct stat *a, const struct stat *b)
{
	if (a->st_dev != b->st_dev || a->st_ino != b->st_ino || a->st_size != b->st_size ||
	    a->st_mode != b->st_mode || a->st_nlink != b->st_nlink || a->st_mtime != b->st_mtime ||
	    a->st_ctime != b->st_ctime)
		return 0;
#ifdef __APPLE__
	return a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
	       a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
	return a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}

static int checkpoint_read_at(int parent, const char *leaf, size_t limit,
			      struct checkpoint_state *out)
{
	struct stat before;
	if (fstatat(parent, leaf, &before, AT_SYMLINK_NOFOLLOW) != 0)
		return errno == ENOENT ? 0 : nb_status_from_errno(errno, NULL);
	if (!S_ISREG(before.st_mode) || before.st_nlink != 1)
		return NB_ERR_LOOP;
	if (before.st_size < 0 || (uint64_t)before.st_size > CHECKPOINT_FILE_MAX_BYTES ||
	    (uint64_t)before.st_size > limit)
		return NB_ERR_TOO_BIG;
	int fd NB_CLEANUP(checkpoint_fd_cleanup) =
		openat(parent, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return nb_status_from_errno(errno, NULL);
	struct stat opened;
	if (fstat(fd, &opened) != 0)
		return nb_status_from_errno(errno, NULL);
	if (!checkpoint_same_stat(&before, &opened) || !S_ISREG(opened.st_mode) ||
	    opened.st_nlink != 1)
		return NB_ERR_AGAIN;
	out->exists = 1;
	out->mode = opened.st_mode & 07777;
	out->size = (size_t)before.st_size;
	if (out->size) {
		out->bytes = malloc(out->size);
		if (!out->bytes)
			return NB_ERR_NOMEM;
	}
	size_t read_bytes = 0;
	while (read_bytes < out->size) {
		ssize_t got = read(fd, out->bytes + read_bytes, out->size - read_bytes);
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0)
			return nb_status_from_errno(errno, NULL);
		if (got == 0)
			return NB_ERR_AGAIN;
		read_bytes += (size_t)got;
	}
	unsigned char extra;
	ssize_t extra_count;
	do {
		extra_count = read(fd, &extra, 1);
	} while (extra_count < 0 && errno == EINTR);
	if (extra_count < 0)
		return nb_status_from_errno(errno, NULL);
	if (extra_count)
		return NB_ERR_AGAIN;
	struct stat after;
	if (fstat(fd, &after) != 0)
		return nb_status_from_errno(errno, NULL);
	if (!checkpoint_same_stat(&after, &opened))
		return NB_ERR_AGAIN;
	if (fstatat(parent, leaf, &after, AT_SYMLINK_NOFOLLOW) != 0)
		return nb_status_from_errno(errno, NULL);
	if (!checkpoint_same_stat(&after, &opened))
		return NB_ERR_AGAIN;
	return 0;
}

static int checkpoint_read(const char *root, const char *path, size_t limit,
			   struct checkpoint_state *out)
{
	int parent NB_CLEANUP(checkpoint_fd_cleanup) = -1;
	char *leaf NB_CLEANUP(nb_charp_cleanup) = NULL;
	int rc = checkpoint_walk(root, path, &parent, &leaf);
	return rc == NB_ERR_NOT_FOUND ? 0
	       : rc != 0	      ? rc
				      : checkpoint_read_at(parent, leaf, limit, out);
}

static int checkpoint_current_size(const char *root, const char *path, size_t *size)
{
	int parent NB_CLEANUP(checkpoint_fd_cleanup) = -1;
	char *leaf NB_CLEANUP(nb_charp_cleanup) = NULL;
	struct stat info;
	int rc = checkpoint_walk(root, path, &parent, &leaf);

	*size = 0;
	if (rc == NB_ERR_NOT_FOUND)
		return 0;
	if (rc != 0)
		return rc;
	if (fstatat(parent, leaf, &info, AT_SYMLINK_NOFOLLOW) != 0)
		return errno == ENOENT ? 0 : nb_status_from_errno(errno, NULL);
	if (!S_ISREG(info.st_mode) || info.st_nlink != 1)
		return NB_ERR_LOOP;
	if (info.st_size < 0 || (uint64_t)info.st_size > CHECKPOINT_FILE_MAX_BYTES)
		return NB_ERR_TOO_BIG;
	*size = (size_t)info.st_size;
	return 0;
}

static int checkpoint_snapshot(struct db *db, int64_t id, int64_t session_id)
{
	struct checkpoint_table tables[10];
	int count = checkpoint_tables(tables, sizeof(tables) / sizeof(tables[0]));
	for (int i = 0; i < count; i++) {
		int present = checkpoint_table_exists(db, tables[i].name);
		if (present < 0)
			return present;
		if (!present)
			continue;
		nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {0};
		sqlite3_stmt *stmt NB_CLEANUP(checkpoint_stmt_cleanup) = NULL;
		int rc = (nb_buf_init(&sql), nb_buf_reserve(&sql, 256) ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_printf(&sql,
					    "INSERT INTO checkpoint_rows_%s SELECT ?1,t.* FROM %s "
					    "t WHERE %s",
					    tables[i].name, tables[i].name, tables[i].filter)
				      ? 0
				      : NB_ERR_NOMEM);
		if (rc == 0)
			rc = checkpoint_prepare(db, nb_buf_cstr(&sql), &stmt);
		if (rc != 0)
			return rc;
		sqlite3_bind_int64(stmt, 1, id);
		sqlite3_bind_int64(stmt, 2, session_id);
		rc = checkpoint_done(stmt);
		if (rc != 0)
			return rc;
	}
	return 0;
}

static int checkpoint_retention(struct db *db, int64_t session_id)
{
	sqlite3_stmt *stmt NB_CLEANUP(checkpoint_stmt_cleanup) = NULL;
	int rc = checkpoint_prepare(
		db,
		"DELETE FROM code_checkpoints WHERE session_id=?1 AND "
		"(created_at<strftime('%s','now')-2592000 OR id NOT IN "
		"(SELECT id FROM code_checkpoints WHERE session_id=?1 ORDER BY id DESC LIMIT 100))",
		&stmt);
	if (rc != 0)
		return rc;
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = checkpoint_done(stmt);
	if (rc == 0)
		rc = db_exec(db, "DELETE FROM code_checkpoint_files WHERE checkpoint_id NOT "
				 "IN(SELECT id FROM code_checkpoints)");
	struct checkpoint_table tables[10];
	int count = checkpoint_tables(tables, sizeof(tables) / sizeof(tables[0]));
	for (int i = 0; rc == 0 && i < count; i++) {
		int present = checkpoint_table_exists(db, tables[i].name);
		if (present < 0)
			return present;
		if (!present)
			continue;
		nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {0};
		rc = (nb_buf_init(&sql), nb_buf_reserve(&sql, 256) ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_printf(&sql,
					    "DELETE FROM checkpoint_rows_%s WHERE checkpoint_id "
					    "NOT IN(SELECT id FROM code_checkpoints)",
					    tables[i].name)
				      ? 0
				      : NB_ERR_NOMEM);
		if (rc == 0)
			rc = db_exec(db, nb_buf_cstr(&sql));
	}
	return rc;
}

int checkpoint_store_create(struct db *db, int64_t session_id, const char *workdir,
			    const char *turn_id, const char *prompt, int64_t *id)
{
	struct checkpoint_lock lock NB_CLEANUP(checkpoint_lock_cleanup) = {0};
	char *root NB_CLEANUP(nb_charp_cleanup) = NULL;
	sqlite3_stmt *stmt NB_CLEANUP(checkpoint_stmt_cleanup) = NULL;
	if (session_id <= 0 || !workdir || !*workdir || !id)
		return NB_ERR_INVAL;
	*id = 0;
	root = realpath(workdir, NULL);
	if (!root)
		return nb_status_from_errno(errno, NULL);
	struct stat info;
	if (stat(root, &info) != 0)
		return nb_status_from_errno(errno, NULL);
	if (!S_ISDIR(info.st_mode))
		return NB_ERR_NOT_DIR;
	int rc = checkpoint_enter(db, &lock);
	if (rc != 0)
		return rc;
	rc = db_exec(db, "SAVEPOINT morph_checkpoint");
	if (rc != 0)
		return rc;
	rc = checkpoint_prepare(
		db,
		"INSERT INTO code_checkpoints(session_id,workspace_root,turn_id,prompt,created_at) "
		"VALUES(?,?,?,?,strftime('%s','now'))",
		&stmt);
	if (rc == 0) {
		sqlite3_bind_int64(stmt, 1, session_id);
		sqlite3_bind_text(stmt, 2, root, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, turn_id ? turn_id : "", -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, prompt ? prompt : "", -1, SQLITE_TRANSIENT);
		rc = checkpoint_done(stmt);
	}
	int64_t created = rc == 0 ? sqlite3_last_insert_rowid(db->handle) : 0;
	checkpoint_stmt_cleanup(&stmt);
	if (rc == 0)
		rc = checkpoint_snapshot(db, created, session_id);
	if (rc == 0)
		rc = checkpoint_retention(db, session_id);
	rc = checkpoint_transaction_finish(db, rc);
	if (rc == 0)
		*id = created;
	return rc;
}

int checkpoint_store_capture(struct db *db, int64_t id, const char *path)
{
	struct checkpoint_lock lock NB_CLEANUP(checkpoint_lock_cleanup) = {0};
	struct checkpoint_info info NB_CLEANUP(checkpoint_info_cleanup) = {0};
	struct checkpoint_state current NB_CLEANUP(checkpoint_state_cleanup) = {0};
	struct checkpoint_state expected NB_CLEANUP(checkpoint_state_cleanup) = {0};
	char *relative NB_CLEANUP(nb_charp_cleanup) = NULL;
	sqlite3_stmt *stmt NB_CLEANUP(checkpoint_stmt_cleanup) = NULL;
	if (id <= 0 || !path)
		return NB_ERR_INVAL;
	int rc = checkpoint_enter(db, &lock);
	if (rc == 0)
		rc = checkpoint_info_read(db, 0, id, &info);
	if (rc != 0)
		return rc;
	if (!info.code_active)
		return NB_ERR_STALE;
	rc = checkpoint_relative(info.root, path, &relative);
	if (rc == 0)
		rc = checkpoint_read(info.root, relative, CHECKPOINT_FILE_MAX_BYTES, &current);
	if (rc != 0)
		return rc;
	rc = db_exec(db, "SAVEPOINT morph_checkpoint");
	if (rc != 0)
		return rc;
	rc = checkpoint_prepare(
		db,
		"SELECT f.checkpoint_id,f.after_exists,f.after_mode,f.after_data,"
		"f.before_exists,f.before_mode,f.before_data,f.sealed,f.pending_token FROM "
		"code_checkpoint_files f "
		"JOIN code_checkpoints c ON c.id=f.checkpoint_id WHERE c.session_id=? AND "
		"c.workspace_root=? "
		"AND c.code_active=1 AND c.id<=? AND f.path=? ORDER BY c.id DESC LIMIT 1",
		&stmt);
	int64_t previous = 0;
	if (rc == 0) {
		sqlite3_bind_int64(stmt, 1, info.session_id);
		sqlite3_bind_text(stmt, 2, info.root, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 3, id);
		sqlite3_bind_text(stmt, 4, relative, -1, SQLITE_TRANSIENT);
		int step = sqlite3_step(stmt);
		if (step == SQLITE_ROW) {
			previous = sqlite3_column_int64(stmt, 0);
			rc = checkpoint_state_read_sql(stmt, sqlite3_column_int(stmt, 7) ? 1 : 4,
						       &expected);
		} else if (step != SQLITE_DONE) {
			rc = MORPH_ERR_DB;
		}
	}
	checkpoint_stmt_cleanup(&stmt);
	if (rc == 0 && previous != id) {
		rc = checkpoint_prepare(db,
					"SELECT "
					"COALESCE(sum(COALESCE(length(before_data),0)+COALESCE("
					"length(after_data),0)),0),count(*) "
					"FROM code_checkpoint_files WHERE checkpoint_id=?",
					&stmt);
		if (rc == 0) {
			sqlite3_bind_int64(stmt, 1, id);
			int step = sqlite3_step(stmt);
			if (step != SQLITE_ROW) {
				rc = MORPH_ERR_DB;
			} else if (sqlite3_column_int64(stmt, 0) < 0 ||
				   (uint64_t)sqlite3_column_int64(stmt, 0) + 2 * current.size >
					   CHECKPOINT_DATA_MAX_BYTES ||
				   sqlite3_column_int64(stmt, 1) >= CHECKPOINT_PLAN_MAX_FILES) {
				rc = NB_ERR_TOO_BIG;
			}
		}
		checkpoint_stmt_cleanup(&stmt);
	}
	if (rc == 0 && previous && !checkpoint_state_equal(&current, &expected)) {
		if (previous == id) {
			rc = NB_ERR_AGAIN;
		} else {
			rc = checkpoint_prepare(
				db,
				"UPDATE code_checkpoints SET code_active=0,"
				"reason='External changes preceded a later checkpoint; older code "
				"history is no longer safe.' "
				"WHERE session_id=? AND workspace_root=? AND id<? AND EXISTS"
				"(SELECT 1 FROM code_checkpoint_files WHERE "
				"checkpoint_id=code_checkpoints.id)",
				&stmt);
			if (rc == 0) {
				sqlite3_bind_int64(stmt, 1, info.session_id);
				sqlite3_bind_text(stmt, 2, info.root, -1, SQLITE_TRANSIENT);
				sqlite3_bind_int64(stmt, 3, id);
				rc = checkpoint_done(stmt);
			}
			checkpoint_stmt_cleanup(&stmt);
		}
	}
	if (rc == 0) {
		rc = checkpoint_prepare(
			db,
			"INSERT INTO code_checkpoint_files(checkpoint_id,path,"
			"before_exists,before_mode,before_data,after_exists,after_mode,after_data,"
			"pending_token) "
			"VALUES(?1,?2,?3,?4,?5,?3,?4,?5,(SELECT token FROM "
			"checkpoint_connection_token)) "
			"ON CONFLICT(checkpoint_id,path) DO UPDATE SET pending_token=(SELECT token "
			"FROM checkpoint_connection_token)",
			&stmt);
		if (rc == 0) {
			sqlite3_bind_int64(stmt, 1, id);
			sqlite3_bind_text(stmt, 2, relative, -1, SQLITE_TRANSIENT);
			rc = checkpoint_state_bind(stmt, 3, &current);
			if (rc == 0)
				rc = checkpoint_done(stmt);
		}
	}
	checkpoint_stmt_cleanup(&stmt);
	return checkpoint_transaction_finish(db, rc);
}

int checkpoint_store_validate_write(struct db *db, int64_t id, const char *path,
				    size_t bytes, const char *removed_path)
{
	struct checkpoint_lock lock NB_CLEANUP(checkpoint_lock_cleanup) = {0};
	struct checkpoint_info info NB_CLEANUP(checkpoint_info_cleanup) = {0};
	char *relative NB_CLEANUP(nb_charp_cleanup) = NULL;
	char *removed NB_CLEANUP(nb_charp_cleanup) = NULL;
	sqlite3_stmt *rows NB_CLEANUP(checkpoint_stmt_cleanup) = NULL;
	size_t total = 0;
	int found = 0;
	int found_removed = !removed_path;
	int rc;

	if (id <= 0 || !path)
		return NB_ERR_INVAL;
	if (bytes > CHECKPOINT_FILE_MAX_BYTES)
		return NB_ERR_TOO_BIG;
	rc = checkpoint_enter(db, &lock);
	if (rc == 0)
		rc = checkpoint_info_read(db, 0, id, &info);
	if (rc != 0)
		return rc;
	if (!info.code_active)
		return NB_ERR_STALE;
	rc = checkpoint_relative(info.root, path, &relative);
	if (rc == 0 && removed_path)
		rc = checkpoint_relative(info.root, removed_path, &removed);
	if (rc != 0)
		return rc;
	if (removed && !strcmp(relative, removed))
		found_removed = 1;
	rc = checkpoint_prepare(db,
		"SELECT path,COALESCE(length(before_data),0),COALESCE(length(after_data),0),"
		"pending_token IS NOT NULL,pending_token=(SELECT token FROM "
		"checkpoint_connection_token) FROM code_checkpoint_files WHERE checkpoint_id=?",
		&rows);
	if (rc != 0)
		return rc;
	sqlite3_bind_int64(rows, 1, id);
	int step;
	while ((step = sqlite3_step(rows)) == SQLITE_ROW) {
		const char *row_path = (const char *)sqlite3_column_text(rows, 0);
		int64_t before = sqlite3_column_int64(rows, 1);
		int64_t recorded_after = sqlite3_column_int64(rows, 2);
		size_t after;

		if (!row_path || before < 0 || recorded_after < 0)
			return MORPH_ERR_DB;
		if (!strcmp(row_path, relative)) {
			if (!sqlite3_column_int(rows, 4))
				return NB_ERR_STALE;
			found = 1;
			after = bytes;
		} else if (removed && !strcmp(row_path, removed)) {
			if (!sqlite3_column_int(rows, 4))
				return NB_ERR_STALE;
			found_removed = 1;
			after = 0;
		} else if (sqlite3_column_int(rows, 3)) {
			rc = checkpoint_current_size(info.root, row_path, &after);
			if (rc != 0)
				return rc;
		} else {
			after = (size_t)recorded_after;
		}
		if ((uint64_t)before > CHECKPOINT_DATA_MAX_BYTES - total)
			return NB_ERR_TOO_BIG;
		total += (size_t)before;
		if (after > CHECKPOINT_DATA_MAX_BYTES - total)
			return NB_ERR_TOO_BIG;
		total += after;
	}
	if (step != SQLITE_DONE)
		return MORPH_ERR_DB;
	return found && found_removed ? 0 : NB_ERR_STALE;
}

static int checkpoint_validate_data_budget(struct db *db, int64_t id)
{
	sqlite3_stmt *row NB_CLEANUP(checkpoint_stmt_cleanup) = NULL;
	int rc = checkpoint_prepare(db,
		"SELECT COALESCE(sum(COALESCE(length(before_data),0)+"
		"COALESCE(length(after_data),0)),0),count(*) FROM code_checkpoint_files "
		"WHERE checkpoint_id=?", &row);

	if (rc != 0)
		return rc;
	sqlite3_bind_int64(row, 1, id);
	if (sqlite3_step(row) != SQLITE_ROW)
		return MORPH_ERR_DB;
	int64_t total = sqlite3_column_int64(row, 0);
	int64_t count = sqlite3_column_int64(row, 1);
	if (total < 0 || (uint64_t)total > CHECKPOINT_DATA_MAX_BYTES ||
	    count < 0 || (uint64_t)count > CHECKPOINT_PLAN_MAX_FILES)
		return NB_ERR_TOO_BIG;
	return 0;
}

int checkpoint_store_seal(struct db *db, int64_t id)
{
	struct checkpoint_lock lock NB_CLEANUP(checkpoint_lock_cleanup) = {0};
	struct checkpoint_info info NB_CLEANUP(checkpoint_info_cleanup) = {0};
	sqlite3_stmt *rows NB_CLEANUP(checkpoint_stmt_cleanup) = NULL;
	if (id <= 0)
		return NB_ERR_INVAL;
	int rc = checkpoint_enter(db, &lock);
	if (rc == 0)
		rc = checkpoint_info_read(db, 0, id, &info);
	if (rc != 0)
		return rc;
	if (!info.code_active)
		return NB_ERR_STALE;
	rc = db_exec(db, "SAVEPOINT morph_checkpoint");
	if (rc != 0)
		return rc;
	rc = checkpoint_prepare(db,
				"SELECT path FROM code_checkpoint_files WHERE checkpoint_id=? "
				"AND pending_token=(SELECT token FROM checkpoint_connection_token)",
				&rows);
	if (rc == 0)
		sqlite3_bind_int64(rows, 1, id);
	int step = SQLITE_DONE;
	while (rc == 0 && (step = sqlite3_step(rows)) == SQLITE_ROW) {
		struct checkpoint_state state NB_CLEANUP(checkpoint_state_cleanup) = {0};
		char *path NB_CLEANUP(nb_charp_cleanup) =
			strdup((const char *)sqlite3_column_text(rows, 0));
		sqlite3_stmt *stmt NB_CLEANUP(checkpoint_stmt_cleanup) = NULL;
		if (!path) {
			rc = NB_ERR_NOMEM;
			break;
		}
		rc = checkpoint_read(info.root, path, CHECKPOINT_FILE_MAX_BYTES, &state);
		if (rc == 0)
			rc = checkpoint_prepare(
				db,
				"UPDATE code_checkpoint_files SET "
				"after_exists=?,after_mode=?,after_data=?,"
				"sealed=1,pending_token=NULL WHERE checkpoint_id=? AND path=?",
				&stmt);
		if (rc == 0)
			rc = checkpoint_state_bind(stmt, 1, &state);
		if (rc == 0) {
			sqlite3_bind_int64(stmt, 4, id);
			sqlite3_bind_text(stmt, 5, path, -1, SQLITE_TRANSIENT);
			rc = checkpoint_done(stmt);
		}
	}
	if (rc == 0 && step != SQLITE_DONE)
		rc = MORPH_ERR_DB;
	checkpoint_stmt_cleanup(&rows);
	if (rc == 0) {
		rc = checkpoint_prepare(
			db,
			"DELETE FROM code_checkpoint_files WHERE checkpoint_id=? "
			"AND sealed=1 AND pending_token IS NULL AND before_exists=after_exists "
			"AND (before_exists=0 OR (before_mode=after_mode AND before_data IS "
			"after_data))",
			&rows);
		if (rc == 0) {
			sqlite3_bind_int64(rows, 1, id);
			rc = checkpoint_done(rows);
		}
		checkpoint_stmt_cleanup(&rows);
	}
	if (rc == 0)
		rc = checkpoint_validate_data_budget(db, id);
	rc = checkpoint_transaction_finish(db, rc);
	/* Failed seals must not leave their token eligible for a later attempt. */
	int rotate_rc = db_exec(
		db, "UPDATE checkpoint_connection_token SET token=lower(hex(randomblob(16)))");
	if (rc != 0) {
		(void)checkpoint_prepare(db,
					 "UPDATE code_checkpoints SET code_active=0,"
					 "reason='An editing attempt could not be sealed; code "
					 "restoration is unsafe.' WHERE id=?",
					 &rows);
		if (rows) {
			sqlite3_bind_int64(rows, 1, id);
			(void)checkpoint_done(rows);
		}
	}
	return rc != 0 ? rc : rotate_rc;
}

static int checkpoint_plan_load(struct db *db, int64_t session_id, int64_t id,
				struct checkpoint_plan *plan)
{
	int rc = checkpoint_info_read(db, session_id, id, &plan->info);
	if (rc == 0)
		rc = (nb_vec_init(&plan->files, sizeof(struct checkpoint_file)),
		      nb_vec_reserve(&plan->files, 8) ? 0 : NB_ERR_NOMEM);
	if (rc != 0 || !plan->info.code_active)
		return rc;
	sqlite3_stmt *stmt NB_CLEANUP(checkpoint_stmt_cleanup) = NULL;
	rc = checkpoint_prepare(
		db,
		"SELECT f.path,f.before_exists,f.before_mode,f.before_data,"
		"f.after_exists,f.after_mode,f.after_data,f.sealed,f.pending_token IS NOT NULL "
		"FROM code_checkpoint_files f JOIN code_checkpoints c ON c.id=f.checkpoint_id "
		"WHERE c.session_id=? AND c.workspace_root=? AND c.id>=? AND c.code_active=1 ORDER "
		"BY c.id,f.path",
		&stmt);
	if (rc != 0)
		return rc;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, plan->info.root, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, id);
	int step;
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *path = (const char *)sqlite3_column_text(stmt, 0);
		struct checkpoint_file *file = NULL;
		for (size_t i = 0; i < plan->files.len; i++) {
			struct checkpoint_file *candidate = nb_vec_at(&plan->files, i);
			if (!strcmp(candidate->path, path)) {
				file = candidate;
				break;
			}
		}
		if (!file) {
			if (plan->files.len >= CHECKPOINT_PLAN_MAX_FILES)
				return NB_ERR_TOO_BIG;
			file = nb_vec_push(&plan->files);
			if (!file)
				return NB_ERR_NOMEM;
			memset(file, 0, sizeof(*file));
			file->path = strdup(path);
			if (!file->path)
				return NB_ERR_NOMEM;
			int size = sqlite3_column_bytes(stmt, 3);
			if (size < 0 || (size_t)size > CHECKPOINT_PLAN_MAX_BYTES - plan->bytes)
				return NB_ERR_TOO_BIG;
			rc = checkpoint_state_read_sql(stmt, 1, &file->before);
			if (rc != 0)
				return rc;
			plan->bytes += file->before.size;
		}
		plan->bytes -= file->after.size;
		checkpoint_state_cleanup(&file->after);
		int size = sqlite3_column_bytes(stmt, 6);
		if (size < 0 || (size_t)size > CHECKPOINT_PLAN_MAX_BYTES - plan->bytes)
			return NB_ERR_TOO_BIG;
		rc = checkpoint_state_read_sql(stmt, 4, &file->after);
		if (rc != 0)
			return rc;
		plan->bytes += file->after.size;
		file->sealed = sqlite3_column_int(stmt, 7);
		file->pending = sqlite3_column_int(stmt, 8);
	}
	if (step != SQLITE_DONE)
		return MORPH_ERR_DB;
	for (size_t i = 0; i < plan->files.len;) {
		struct checkpoint_file *file = nb_vec_at(&plan->files, i);
		if (file->sealed && !file->pending &&
		    checkpoint_state_equal(&file->before, &file->after)) {
			plan->bytes -= file->before.size + file->after.size;
			free(file->path);
			checkpoint_state_cleanup(&file->before);
			checkpoint_state_cleanup(&file->after);
			nb_vec_remove(&plan->files, i);
		} else {
			i++;
		}
	}
	return 0;
}

static int checkpoint_file_compare(const void *a, const void *b)
{
	const struct checkpoint_file *left = a;
	const struct checkpoint_file *right = b;
	return strcmp(left->path, right->path);
}

static int checkpoint_plan_check(struct checkpoint_plan *plan)
{
	if (plan->files.len > 1)
		qsort(((void *)plan->files.data), plan->files.len, sizeof(struct checkpoint_file),
		      checkpoint_file_compare);
	struct checkpoint_file *file;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&plan->files) &&
			     (file = (struct checkpoint_file *)nb_vec_at(&plan->files, _i_));
	     _i_++) {
		char *normalized NB_CLEANUP(nb_charp_cleanup) = NULL;
		int rc = checkpoint_relative(plan->info.root, file->path, &normalized);
		if (rc == 0 && strcmp(normalized, file->path))
			rc = NB_ERR_PERMISSION;
		if (rc == 0)
			rc = checkpoint_read(plan->info.root, file->path,
					     CHECKPOINT_PLAN_MAX_BYTES - plan->bytes,
					     &file->current);
		if (rc == NB_ERR_NOMEM)
			return rc;
		if (file->current.size > CHECKPOINT_PLAN_MAX_BYTES - plan->bytes)
			return NB_ERR_TOO_BIG;
		plan->bytes += file->current.size;
		file->conflict = rc;
		if (rc == 0 && (!file->sealed || file->pending))
			file->conflict = NB_ERR_IN_PROGRESS;
		if (rc == 0 && !file->conflict &&
		    !checkpoint_state_equal(&file->current, &file->after))
			file->conflict = NB_ERR_AGAIN;
	}
	return 0;
}

static int checkpoint_conflict_count(const struct checkpoint_plan *plan)
{
	int count = 0;
	for (size_t i = 0; i < plan->files.len; i++) {
		const struct checkpoint_file *file = nb_vec_at(&plan->files, i);
		count += file->conflict != 0;
	}
	return count;
}

static int checkpoint_json_info(cJSON *json, const struct checkpoint_info *info)
{
	return cJSON_AddNumberToObject(json, "id", (double)info->id) &&
	       cJSON_AddStringToObject(json, "turn_id", info->turn_id) &&
	       cJSON_AddStringToObject(json, "prompt", info->prompt) &&
	       cJSON_AddStringToObject(json, "workspace_root", info->root) &&
	       cJSON_AddNumberToObject(json, "created_at", (double)info->created_at) &&
	       cJSON_AddBoolToObject(json, "code_active", info->code_active) &&
	       cJSON_AddBoolToObject(json, "conversation_restorable", info->conversation_active) &&
	       cJSON_AddStringToObject(json, "reason", info->reason);
}

static const char *checkpoint_conflict_reason(int error)
{
	if (error == NB_ERR_IN_PROGRESS)
		return "An editing attempt was not sealed; its final file state is unknown.";
	if (error == NB_ERR_AGAIN)
		return "The file changed outside the captured editing history.";
	if (error == NB_ERR_LOOP || error == NB_ERR_NOT_DIR)
		return "A symlink, hardlink, or non-regular path prevents safe restoration.";
	if (error == NB_ERR_TOO_BIG)
		return "Checkpoint size limits exceeded (16 MiB/file, 128 MiB captured data, "
		       "256 MiB restore plan including compensation copies).";
	return nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, error);
}

static cJSON *checkpoint_report(const struct checkpoint_plan *plan)
{
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!report || !checkpoint_json_info(report, &plan->info))
		return NULL;
	cJSON *files = cJSON_AddArrayToObject(report, "files");
	cJSON *conflicts = cJSON_AddArrayToObject(report, "conflicts");
	if (!files || !conflicts ||
	    !cJSON_AddBoolToObject(report, "code_restorable",
				   plan->info.code_active && !checkpoint_conflict_count(plan)))
		return NULL;
	for (size_t i = 0; i < plan->files.len; i++) {
		const struct checkpoint_file *file = nb_vec_at(&plan->files, i);
		cJSON *row NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		if (!row || !cJSON_AddStringToObject(row, "path", file->path) ||
		    !cJSON_AddBoolToObject(row, "before_exists", file->before.exists) ||
		    !cJSON_AddBoolToObject(row, "restore_exists", file->before.exists) ||
		    !cJSON_AddBoolToObject(row, "after_exists", file->after.exists) ||
		    !cJSON_AddBoolToObject(row, "current_exists", file->current.exists) ||
		    !cJSON_AddNumberToObject(row, "before_bytes", (double)file->before.size) ||
		    !cJSON_AddNumberToObject(row, "current_bytes", (double)file->current.size) ||
		    !cJSON_AddNumberToObject(row, "restore_mode", (double)file->before.mode) ||
		    !cJSON_AddBoolToObject(row, "conflict", file->conflict != 0))
			return NULL;
		if ((cJSON_AddItemToArray(files, row) ? (row = NULL, NB_OK) : NB_ERR_NOMEM) != 0)
			return NULL;
		if (file->conflict) {
			row = cJSON_CreateObject();
			if (!row || !cJSON_AddStringToObject(row, "path", file->path) ||
			    !cJSON_AddStringToObject(row, "reason",
						     checkpoint_conflict_reason(file->conflict)) ||
			    !cJSON_AddNumberToObject(row, "error_code", file->conflict) ||
			    (cJSON_AddItemToArray(conflicts, row) ? (row = NULL, NB_OK)
								  : NB_ERR_NOMEM) != 0)
				return NULL;
		}
	}
	cJSON *value = report;
	report = NULL;
	return value;
}

int checkpoint_store_list_copy(struct db *db, int64_t session_id, cJSON **out)
{
	struct checkpoint_lock lock NB_CLEANUP(checkpoint_lock_cleanup) = {0};
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	sqlite3_stmt *stmt NB_CLEANUP(checkpoint_stmt_cleanup) = NULL;
	if (session_id <= 0 || !out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	int rc = checkpoint_enter(db, &lock);
	if (rc != 0)
		return rc;
	report = cJSON_CreateObject();
	cJSON *rows = report ? cJSON_AddArrayToObject(report, "checkpoints") : NULL;
	if (!rows)
		return NB_ERR_NOMEM;
	rc = checkpoint_prepare(
		db,
		"SELECT c.id,(SELECT count(*) FROM code_checkpoint_files WHERE checkpoint_id=c.id) "
		"FROM code_checkpoints c WHERE c.session_id=? AND "
		"created_at>=strftime('%s','now')-2592000 ORDER BY c.id DESC",
		&stmt);
	if (rc != 0)
		return rc;
	sqlite3_bind_int64(stmt, 1, session_id);
	int step;
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		struct checkpoint_info info NB_CLEANUP(checkpoint_info_cleanup) = {0};
		cJSON *row NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		rc = checkpoint_info_read(db, session_id, sqlite3_column_int64(stmt, 0), &info);
		if (rc != 0)
			return rc;
		if (!row || !checkpoint_json_info(row, &info) ||
		    !cJSON_AddNumberToObject(row, "files", (double)sqlite3_column_int64(stmt, 1)) ||
		    !cJSON_AddBoolToObject(row, "code_restorable", info.code_active) ||
		    (cJSON_AddItemToArray(rows, row) ? (row = NULL, NB_OK) : NB_ERR_NOMEM) != 0)
			return NB_ERR_NOMEM;
	}
	if (step != SQLITE_DONE)
		return MORPH_ERR_DB;
	*out = report;
	report = NULL;
	return 0;
}

int checkpoint_store_preview_copy(struct db *db, int64_t session_id, int64_t id, cJSON **out)
{
	struct checkpoint_lock lock NB_CLEANUP(checkpoint_lock_cleanup) = {0};
	struct checkpoint_plan plan NB_CLEANUP(checkpoint_plan_cleanup) = {0};
	if (session_id <= 0 || id <= 0 || !out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	int rc = checkpoint_enter(db, &lock);
	if (rc == 0)
		rc = checkpoint_plan_load(db, session_id, id, &plan);
	if (rc == 0)
		rc = checkpoint_plan_check(&plan);
	if (rc != 0)
		return rc;
	*out = checkpoint_report(&plan);
	return *out ? 0 : NB_ERR_NOMEM;
}

int checkpoint_store_info_copy(struct db *db, int64_t session_id, int64_t id, cJSON **out)
{
	struct checkpoint_lock lock NB_CLEANUP(checkpoint_lock_cleanup) = {0};
	struct checkpoint_info info NB_CLEANUP(checkpoint_info_cleanup) = {0};
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	if (!out || session_id <= 0 || id <= 0)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	int rc = checkpoint_enter(db, &lock);
	if (rc == 0)
		rc = checkpoint_info_read(db, session_id, id, &info);
	if (rc != 0)
		return rc;
	report = cJSON_CreateObject();
	if (!report || !checkpoint_json_info(report, &info))
		return NB_ERR_NOMEM;
	*out = report;
	report = NULL;
	return 0;
}

static int checkpoint_write_bytes(int fd, const struct checkpoint_state *state)
{
	size_t written = 0;
	while (written < state->size) {
		ssize_t count = write(fd, state->bytes + written, state->size - written);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			return nb_status_from_errno(errno, NULL);
		if (!count)
			return NB_ERR_IO;
		written += (size_t)count;
	}
	if (fchmod(fd, state->mode) != 0 || fsync(fd) != 0)
		return nb_status_from_errno(errno, NULL);
	return 0;
}

static int checkpoint_replace_recover(int parent, const char *leaf,
				      const char *backup, const char *staging,
				      int moved, int *changed, int *staging_owned, int rc)
{
	if (moved && linkat(parent, backup, parent, leaf, 0) == 0) {
		if (unlinkat(parent, backup, 0) == 0)
			*changed = 0;
	}
	if (*staging_owned && unlinkat(parent, staging, 0) == 0)
		*staging_owned = 0;
	return rc;
}

/* Backups remain until the database commits. Publishing with linkat refuses
 * to replace a path created by a concurrent external writer. */
static int checkpoint_replace(const char *root, const char *path,
			      const struct checkpoint_state *expected,
			      const struct checkpoint_state *desired, const char *backup,
			      const char *staging, int *changed, int *staging_owned)
{
	*changed = 0;
	*staging_owned = 0;
	int parent NB_CLEANUP(checkpoint_fd_cleanup) = -1;
	int temporary NB_CLEANUP(checkpoint_fd_cleanup) = -1;
	char *leaf NB_CLEANUP(nb_charp_cleanup) = NULL;
	struct checkpoint_state current NB_CLEANUP(checkpoint_state_cleanup) = {0};
	int moved = 0;
	int rc = checkpoint_walk(root, path, &parent, &leaf);
	if (rc == NB_ERR_NOT_FOUND && !expected->exists && !desired->exists)
		return 0;
	if (rc != 0)
		return rc;
	rc = checkpoint_read_at(parent, leaf, CHECKPOINT_FILE_MAX_BYTES, &current);
	if (rc != 0)
		return rc;
	if (!checkpoint_state_equal(&current, expected))
		return NB_ERR_AGAIN;
	if (checkpoint_state_equal(expected, desired))
		return 0;
	if (desired->exists) {
		temporary = openat(parent, staging,
				   O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (temporary < 0)
			return nb_status_from_errno(errno, NULL);
		*staging_owned = 1;
		rc = checkpoint_write_bytes(temporary, desired);
		checkpoint_fd_cleanup(&temporary);
		if (rc != 0)
			return checkpoint_replace_recover(parent, leaf, backup, staging,
						 0, changed, staging_owned, rc);
	}
	if (expected->exists) {
		temporary = openat(parent, backup,
				   O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (temporary < 0) {
			rc = nb_status_from_errno(errno, NULL);
			return checkpoint_replace_recover(parent, leaf, backup, staging,
						 0, changed, staging_owned, rc);
		}
		checkpoint_fd_cleanup(&temporary);
		if (renameat(parent, leaf, parent, backup) != 0) {
			rc = nb_status_from_errno(errno, NULL);
			(void)unlinkat(parent, backup, 0);
			return checkpoint_replace_recover(parent, leaf, backup, staging,
						 0, changed, staging_owned, rc);
		}
		moved = 1;
		*changed = 1;
		checkpoint_state_cleanup(&current);
		rc = checkpoint_read_at(parent, backup, CHECKPOINT_FILE_MAX_BYTES, &current);
		if (rc == 0 && !checkpoint_state_equal(&current, expected))
			rc = NB_ERR_AGAIN;
		if (rc != 0)
			return checkpoint_replace_recover(parent, leaf, backup, staging,
						 moved, changed, staging_owned, rc);
	}
	if (desired->exists && linkat(parent, staging, parent, leaf, 0) != 0) {
		rc = nb_status_from_errno(errno, NULL);
		return checkpoint_replace_recover(parent, leaf, backup, staging,
						 moved, changed, staging_owned, rc);
	}
	*changed = 1;
	if (*staging_owned) {
		if (unlinkat(parent, staging, 0) != 0)
			return nb_status_from_errno(errno, NULL);
		*staging_owned = 0;
	}
	return 0;
}

static int checkpoint_backup_remove(const char *root, const char *path, const char *backup)
{
	if (!backup[0])
		return 0;
	int parent NB_CLEANUP(checkpoint_fd_cleanup) = -1;
	char *leaf NB_CLEANUP(nb_charp_cleanup) = NULL;
	int rc = checkpoint_walk(root, path, &parent, &leaf);
	if (rc != 0)
		return rc;
	if (unlinkat(parent, backup, 0) != 0 && errno != ENOENT)
		return nb_status_from_errno(errno, NULL);
	return 0;
}

static int checkpoint_columns(struct db *db, const char *table, nb_buf *out)
{
	nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {0};
	sqlite3_stmt *stmt NB_CLEANUP(checkpoint_stmt_cleanup) = NULL;
	int rc = (nb_buf_init(&sql), nb_buf_reserve(&sql, 128) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_printf(&sql, "PRAGMA table_info(%s)", table) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = checkpoint_prepare(db, nb_buf_cstr(&sql), &stmt);
	if (rc != 0)
		return rc;
	int step;
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *column = (const char *)sqlite3_column_text(stmt, 1);
		if (out->len)
			rc = (nb_buf_append_char(out, ',') ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_char(out, '"') ? 0 : NB_ERR_NOMEM);
		for (const char *cursor = column; rc == 0 && *cursor; cursor++) {
			rc = (nb_buf_append_char(out, *cursor) ? 0 : NB_ERR_NOMEM);
			if (rc == 0 && *cursor == '"')
				rc = (nb_buf_append_char(out, '"') ? 0 : NB_ERR_NOMEM);
		}
		if (rc == 0)
			rc = (nb_buf_append_char(out, '"') ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
	}
	return step == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int checkpoint_conversation_restore(struct db *db, const struct checkpoint_info *info)
{
	struct checkpoint_table tables[10];
	int count = checkpoint_tables(tables, sizeof(tables) / sizeof(tables[0]));
	for (int i = 0; i < count; i++) {
		int present = checkpoint_table_exists(db, tables[i].name);
		if (present < 0)
			return present;
		if (!present)
			continue;
		nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {0};
		sqlite3_stmt *stmt NB_CLEANUP(checkpoint_stmt_cleanup) = NULL;
		int rc = (nb_buf_init(&sql), nb_buf_reserve(&sql, 256) ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_printf(&sql, "DELETE FROM %s WHERE %s", tables[i].name,
					    tables[i].filter)
				      ? 0
				      : NB_ERR_NOMEM);
		if (rc == 0)
			rc = checkpoint_prepare(db, nb_buf_cstr(&sql), &stmt);
		if (rc != 0)
			return rc;
		sqlite3_bind_int64(stmt, 2, info->session_id);
		rc = checkpoint_done(stmt);
		if (rc != 0)
			return rc;
	}
	for (int i = count - 1; i >= 0; i--) {
		int present = checkpoint_table_exists(db, tables[i].name);
		if (present < 0)
			return present;
		if (!present)
			continue;
		nb_buf columns NB_CLEANUP(nb_buf_cleanup) = {0};
		nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {0};
		sqlite3_stmt *stmt NB_CLEANUP(checkpoint_stmt_cleanup) = NULL;
		int rc = (nb_buf_init(&columns), nb_buf_reserve(&columns, 128) ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = checkpoint_columns(db, tables[i].name, &columns);
		if (rc == 0)
			rc = (nb_buf_init(&sql), nb_buf_reserve(&sql, 256) ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_printf(&sql,
					    "INSERT INTO %s(%s) SELECT %s FROM "
					    "checkpoint_rows_%s WHERE checkpoint_id=?",
					    tables[i].name, nb_buf_cstr(&columns),
					    nb_buf_cstr(&columns), tables[i].name)
				      ? 0
				      : NB_ERR_NOMEM);
		if (rc == 0)
			rc = checkpoint_prepare(db, nb_buf_cstr(&sql), &stmt);
		if (rc != 0)
			return rc;
		sqlite3_bind_int64(stmt, 1, info->id);
		rc = checkpoint_done(stmt);
		if (rc != 0)
			return rc;
	}
	return 0;
}

static int checkpoint_mark_undone(struct db *db, const struct checkpoint_info *info,
				  enum checkpoint_restore_mode mode)
{
	sqlite3_stmt *stmt NB_CLEANUP(checkpoint_stmt_cleanup) = NULL;
	int rc = checkpoint_prepare(
		db,
		"UPDATE code_checkpoints SET "
		"code_active=CASE WHEN ?1 AND workspace_root=?5 AND EXISTS(SELECT 1 FROM "
		"code_checkpoint_files WHERE checkpoint_id=code_checkpoints.id) THEN 0 ELSE "
		"code_active END,"
		"conversation_active=CASE WHEN ?2 THEN 0 ELSE conversation_active END,"
		"reason=CASE WHEN ?1 AND workspace_root=?5 AND EXISTS(SELECT 1 FROM "
		"code_checkpoint_files WHERE checkpoint_id=code_checkpoints.id) "
		"THEN 'Code changes were rewound; this checkpoint belongs to an undone branch.' "
		"ELSE reason END "
		"WHERE session_id=?3 AND id>=?4 AND (workspace_root=?5 OR ?2)",
		&stmt);
	if (rc != 0)
		return rc;
	sqlite3_bind_int(stmt, 1, !!(mode & CHECKPOINT_RESTORE_CODE));
	sqlite3_bind_int(stmt, 2, !!(mode & CHECKPOINT_RESTORE_CONVERSATION));
	sqlite3_bind_int64(stmt, 3, info->session_id);
	sqlite3_bind_int64(stmt, 4, info->id);
	sqlite3_bind_text(stmt, 5, info->root, -1, SQLITE_TRANSIENT);
	return checkpoint_done(stmt);
}

static int checkpoint_effects_prepare(struct checkpoint_plan *plan, cJSON *report)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_2 = NB_OK;
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_3 = NB_OK;
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_4 = NB_OK;

	cJSON *effects = cJSON_AddArrayToObject(report, "effects");
	if (!effects || !cJSON_AddBoolToObject(report, "code_restored", 0) ||
	    !cJSON_AddBoolToObject(report, "conversation_restored", 0) ||
	    !cJSON_AddBoolToObject(report, "compensated", 0) ||
	    !cJSON_AddBoolToObject(report, "rollback_incomplete", 0) ||
	    !cJSON_AddBoolToObject(report, "database_rollback_failed", 0) ||
	    !cJSON_AddBoolToObject(report, "cleanup_incomplete", 0) ||
	    !cJSON_AddNumberToObject(report, "error_code", 0))
		return NB_ERR_NOMEM;
	struct checkpoint_file *file;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&plan->files) &&
			     (file = (struct checkpoint_file *)nb_vec_at(&plan->files, _i_));
	     _i_++) {
		int rc = ((file_status_1 =
				   ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
				    : !(nb_buf_clear(&file_value_1),
					nb_buf_append_cstr(&file_value_1, ".morph-rewind-save-"))
					    ? NB_ERR_NOMEM
					    : nb_random_hex(&file_value_1, 16, NULL))) != NB_OK
				  ? file_status_1
			  : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), file->backup,
							    sizeof(file->backup))) == NB_ERR_RANGE
				  ? NB_ERR_NO_SPACE
				  : file_status_1);
		if (rc == 0)
			rc = ((file_status_2 =
				       ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
					: !(nb_buf_clear(&file_value_2),
					    nb_buf_append_cstr(&file_value_2,
							       ".morph-rewind-undo-"))
						? NB_ERR_NOMEM
						: nb_random_hex(&file_value_2, 16, NULL))) != NB_OK
				      ? file_status_2
			      : (file_status_2 = nb_str_copy_to(
					 nb_buf_str(&file_value_2), file->compensation_backup,
					 sizeof(file->compensation_backup))) == NB_ERR_RANGE
				      ? NB_ERR_NO_SPACE
				      : file_status_2);
		if (rc == 0)
			rc = ((file_status_3 =
				       ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
					: !(nb_buf_clear(&file_value_3),
					    nb_buf_append_cstr(&file_value_3, ".morph-rewind-new-"))
						? NB_ERR_NOMEM
						: nb_random_hex(&file_value_3, 16, NULL))) != NB_OK
				      ? file_status_3
			      : (file_status_3 =
					 nb_str_copy_to(nb_buf_str(&file_value_3), file->staging,
							sizeof(file->staging))) == NB_ERR_RANGE
				      ? NB_ERR_NO_SPACE
				      : file_status_3);
		if (rc == 0)
			rc = ((file_status_4 =
				       ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
					: !(nb_buf_clear(&file_value_4),
					    nb_buf_append_cstr(&file_value_4,
							       ".morph-rewind-comp-"))
						? NB_ERR_NOMEM
						: nb_random_hex(&file_value_4, 16, NULL))) != NB_OK
				      ? file_status_4
			      : (file_status_4 = nb_str_copy_to(
					 nb_buf_str(&file_value_4), file->compensation_staging,
					 sizeof(file->compensation_staging))) == NB_ERR_RANGE
				      ? NB_ERR_NO_SPACE
				      : file_status_4);
		if (rc != 0)
			return rc;
		nb_buf backup_path NB_CLEANUP(nb_buf_cleanup) = {0};
		rc = (nb_buf_init(&backup_path),
		      nb_buf_reserve(&backup_path, strlen(file->path) + sizeof(file->backup))
			      ? 0
			      : NB_ERR_NOMEM);
		const char *slash = strrchr(file->path, '/');
		if (rc == 0 && slash)
			rc = (nb_buf_append(&backup_path, file->path,
					    (size_t)(slash - file->path) + 1)
				      ? 0
				      : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_cstr(&backup_path, file->backup) ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
		cJSON *row NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		if (!row || !cJSON_AddStringToObject(row, "path", file->path) ||
		    !cJSON_AddStringToObject(row, "recovery_path", nb_buf_cstr(&backup_path)) ||
		    !cJSON_AddBoolToObject(row, "applied", 0) ||
		    !cJSON_AddBoolToObject(row, "compensated", 0) ||
		    !cJSON_AddBoolToObject(row, "recovery_available", 0) ||
		    !cJSON_AddStringToObject(row, "staging_basename", file->staging) ||
		    !cJSON_AddStringToObject(row, "compensation_staging_basename",
					     file->compensation_staging) ||
		    !cJSON_AddBoolToObject(row, "staging_remains", 0) ||
		    !cJSON_AddNumberToObject(row, "operation_error", 0) ||
		    !cJSON_AddNumberToObject(row, "cleanup_error", 0) ||
		    !cJSON_AddNumberToObject(row, "compensation_error", 0) ||
		    (cJSON_AddItemToArray(effects, row) ? (row = NULL, NB_OK) : NB_ERR_NOMEM) != 0)
			return NB_ERR_NOMEM;
	}
	return 0;
}

static void checkpoint_json_bool(cJSON *object, const char *name, int value)
{
	cJSON *field = cJSON_GetObjectItemCaseSensitive(object, name);
	if (field)
		cJSON_SetBoolValue(field, value);
}

static int checkpoint_compensate(struct checkpoint_plan *plan, cJSON *report)
{
	cJSON *effects = cJSON_GetObjectItemCaseSensitive(report, "effects");
	int failure = 0;
	for (size_t i = plan->files.len; i > 0; i--) {
		struct checkpoint_file *file = nb_vec_at(&plan->files, i - 1);
		cJSON *row = cJSON_GetArrayItem(effects, (int)(i - 1));
		if (file->staging_owned) {
			int cleanup = checkpoint_backup_remove(plan->info.root, file->path,
							       file->staging);
			if (cleanup) {
				failure = cleanup;
				checkpoint_json_bool(report, "cleanup_incomplete", 1);
				checkpoint_json_bool(row, "staging_remains", 1);
				cJSON_SetNumberValue(
					cJSON_GetObjectItemCaseSensitive(row, "cleanup_error"),
					cleanup);
			} else {
				file->staging_owned = 0;
			}
		}
		if (!file->applied)
			continue;
		int changed = 0;
		int rc = checkpoint_replace(plan->info.root, file->path, &file->before,
					    &file->current, file->compensation_backup,
					    file->compensation_staging, &changed,
					    &file->compensation_staging_owned);
		file->compensated = rc == 0;
		checkpoint_json_bool(row, "compensated", rc == 0);
		if (rc == 0) {
			int cleanup =
				checkpoint_backup_remove(plan->info.root, file->path, file->backup);
			int second = checkpoint_backup_remove(plan->info.root, file->path,
							      file->compensation_backup);
			if (cleanup || second) {
				int cleanup_error = cleanup ? cleanup : second;
				checkpoint_json_bool(report, "cleanup_incomplete", 1);
				checkpoint_json_bool(row, "recovery_available",
						     cleanup && file->current.exists);
				cJSON_SetNumberValue(
					cJSON_GetObjectItemCaseSensitive(row, "cleanup_error"),
					cleanup_error);
			}
		} else {
			failure = rc;
			checkpoint_json_bool(row, "recovery_available", file->current.exists);
			cJSON_SetNumberValue(
				cJSON_GetObjectItemCaseSensitive(row, "compensation_error"), rc);
		}
		if (file->compensation_staging_owned) {
			int cleanup = checkpoint_backup_remove(plan->info.root, file->path,
							       file->compensation_staging);
			if (cleanup) {
				failure = cleanup;
				checkpoint_json_bool(row, "staging_remains", 1);
				checkpoint_json_bool(report, "cleanup_incomplete", 1);
				cJSON_SetNumberValue(
					cJSON_GetObjectItemCaseSensitive(row, "cleanup_error"),
					cleanup);
			}
		}
	}
	checkpoint_json_bool(report, "compensated", !failure);
	checkpoint_json_bool(report, "rollback_incomplete", failure != 0);
	return failure;
}

int checkpoint_store_restore(struct db *db, int64_t session_id, int64_t id,
			     enum checkpoint_restore_mode mode, cJSON **out)
{
	struct checkpoint_lock lock NB_CLEANUP(checkpoint_lock_cleanup) = {0};
	struct checkpoint_plan plan NB_CLEANUP(checkpoint_plan_cleanup) = {0};
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	if (!out || id <= 0 || session_id <= 0 || mode < CHECKPOINT_RESTORE_CODE ||
	    mode > CHECKPOINT_RESTORE_BOTH)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	int rc = checkpoint_enter(db, &lock);
	if (rc == 0) {
		rc = mode & CHECKPOINT_RESTORE_CODE
			     ? checkpoint_plan_load(db, session_id, id, &plan)
			     : checkpoint_info_read(db, session_id, id, &plan.info);
	}
	if (rc == 0 && (mode & CHECKPOINT_RESTORE_CODE))
		rc = checkpoint_plan_check(&plan);
	if (rc != 0)
		return rc;
	report = checkpoint_report(&plan);
	if (!report)
		return NB_ERR_NOMEM;
	rc = checkpoint_effects_prepare(&plan, report);
	if (rc != 0)
		return rc;
	if (((mode & CHECKPOINT_RESTORE_CODE) && !plan.info.code_active) ||
	    ((mode & CHECKPOINT_RESTORE_CONVERSATION) && !plan.info.conversation_active)) {
		rc = NB_ERR_STALE;
	} else if ((mode & CHECKPOINT_RESTORE_CODE) && checkpoint_conflict_count(&plan)) {
		rc = NB_ERR_AGAIN;
	} else {
		rc = db_exec(db, "SAVEPOINT morph_checkpoint");
		if (rc == 0) {
			if (mode & CHECKPOINT_RESTORE_CONVERSATION)
				rc = checkpoint_conversation_restore(db, &plan.info);
			cJSON *effects = cJSON_GetObjectItemCaseSensitive(report, "effects");
			for (size_t i = 0;
			     rc == 0 && (mode & CHECKPOINT_RESTORE_CODE) && i < plan.files.len;
			     i++) {
				struct checkpoint_file *file = nb_vec_at(&plan.files, i);
				rc = checkpoint_replace(plan.info.root, file->path, &file->after,
							&file->before, file->backup, file->staging,
							&file->applied, &file->staging_owned);
				cJSON *effect = cJSON_GetArrayItem(effects, (int)i);
				checkpoint_json_bool(effect, "applied", file->applied);
				cJSON_SetNumberValue(
					cJSON_GetObjectItemCaseSensitive(effect, "operation_error"),
					rc);
			}
			if (rc == 0)
				rc = checkpoint_mark_undone(db, &plan.info, mode);
			if (rc == 0) {
				rc = db_exec(db, "RELEASE morph_checkpoint");
				if (rc == 0) {
					checkpoint_json_bool(report, "code_restored",
							     mode & CHECKPOINT_RESTORE_CODE);
					checkpoint_json_bool(
						report, "conversation_restored",
						mode & CHECKPOINT_RESTORE_CONVERSATION);
					for (size_t i = 0; i < plan.files.len; i++) {
						struct checkpoint_file *file =
							nb_vec_at(&plan.files, i);
						int cleanup = checkpoint_backup_remove(
							plan.info.root, file->path, file->backup);
						cJSON *effect = cJSON_GetArrayItem(effects, (int)i);
						cJSON_SetNumberValue(
							cJSON_GetObjectItemCaseSensitive(
								effect, "cleanup_error"),
							cleanup);
						if (cleanup) {
							checkpoint_json_bool(
								report, "cleanup_incomplete", 1);
							checkpoint_json_bool(effect,
									     "recovery_available",
									     file->current.exists);
						}
					}
				}
			}
			if (rc != 0) {
				int compensation = checkpoint_compensate(&plan, report);
				int rollback =
					sqlite3_get_autocommit(db->handle)
						? 0
						: db_exec(db, "ROLLBACK TO morph_checkpoint; "
							      "RELEASE morph_checkpoint;");
				if (compensation || rollback) {
					checkpoint_json_bool(report, "database_rollback_failed",
							     rollback != 0);
					checkpoint_json_bool(report, "rollback_incomplete", 1);
					rc = NB_ERR_IO;
				}
			}
		}
	}
	cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(report, "error_code"), rc);
	*out = report;
	report = NULL;
	return rc;
}
