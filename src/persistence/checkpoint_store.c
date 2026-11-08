#include "checkpoint_store.h"

#include "util/array.h"
#include "util/buf.h"
#include "util/cleanup.h"
#include "util/error.h"
#include "util/file.h"
#include "util/id.h"
#include "util/json.h"

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

#define CHECKPOINT_FILE_MAX_BYTES (16U * 1024U * 1024U)
#define CHECKPOINT_PLAN_MAX_BYTES (128U * 1024U * 1024U)
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
	morph_array_t files;
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
	morph_array_foreach (file, &plan->files, struct checkpoint_file) {
		free(file->path);
		checkpoint_state_cleanup(&file->before);
		checkpoint_state_cleanup(&file->after);
		checkpoint_state_cleanup(&file->current);
	}
	morph_array_cleanup(&plan->files);
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
	sqlite3_stmt *stmt MORPH_CLEANUP(checkpoint_stmt_cleanup) = NULL;
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
		{"model_history_items", "session_id=?2"},
		{"messages", "session_id=?2"},
	};
	if (capacity < sizeof(source) / sizeof(source[0]))
		return -EOVERFLOW;
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
	struct checkpoint_table tables[9];
	int count = checkpoint_tables(tables, sizeof(tables) / sizeof(tables[0]));
	for (int i = 0; rc == 0 && i < count; i++) {
		int present = checkpoint_table_exists(db, tables[i].name);
		if (present < 0)
			return present;
		if (!present)
			continue;
		morph_buf_t query MORPH_CLEANUP(morph_buf_cleanup) = {0};
		rc = morph_buf_init(&query, 256);
		if (rc == 0)
			rc = morph_buf_printf(
				&query,
				"CREATE TABLE IF NOT EXISTS checkpoint_rows_%s AS "
				"SELECT CAST(0 AS INTEGER) AS checkpoint_id,t.* FROM %s t WHERE 0;"
				"CREATE INDEX IF NOT EXISTS checkpoint_rows_%s_owner ON "
				"checkpoint_rows_%s(checkpoint_id);"
				"CREATE TRIGGER IF NOT EXISTS checkpoint_rows_%s_delete AFTER "
				"DELETE ON code_checkpoints "
				"BEGIN DELETE FROM checkpoint_rows_%s WHERE checkpoint_id=OLD.id; "
				"END;",
				tables[i].name, tables[i].name, tables[i].name, tables[i].name,
				tables[i].name, tables[i].name);
		if (rc == 0)
			rc = db_exec(db, morph_buf_cstr_borrow(&query));
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
		return -EINVAL;
	lock->mutex = sqlite3_db_mutex(db->handle);
	if (lock->mutex)
		sqlite3_mutex_enter(lock->mutex);
	if (!sqlite3_get_autocommit(db->handle))
		return -EBUSY;
	sqlite3_stmt *stmt MORPH_CLEANUP(checkpoint_stmt_cleanup) = NULL;
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
	sqlite3_stmt *stmt MORPH_CLEANUP(checkpoint_stmt_cleanup) = NULL;
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
		return rc == SQLITE_DONE ? -ENOENT : MORPH_ERR_DB;
	out->id = id;
	out->session_id = sqlite3_column_int64(stmt, 0);
	out->root = strdup((const char *)sqlite3_column_text(stmt, 1));
	out->turn_id = strdup((const char *)sqlite3_column_text(stmt, 2));
	out->prompt = strdup((const char *)sqlite3_column_text(stmt, 3));
	out->created_at = sqlite3_column_int64(stmt, 4);
	out->code_active = sqlite3_column_int(stmt, 5);
	out->conversation_active = sqlite3_column_int(stmt, 6);
	out->reason = strdup((const char *)sqlite3_column_text(stmt, 7));
	return out->root && out->turn_id && out->prompt && out->reason ? 0 : -ENOMEM;
}

static int checkpoint_state_read_sql(sqlite3_stmt *stmt, int offset, struct checkpoint_state *out)
{
	out->exists = sqlite3_column_int(stmt, offset);
	out->mode = (mode_t)sqlite3_column_int(stmt, offset + 1);
	int size = sqlite3_column_bytes(stmt, offset + 2);
	if (size < 0 || (unsigned int)size > CHECKPOINT_FILE_MAX_BYTES)
		return -EFBIG;
	out->size = (size_t)size;
	if (size) {
		out->bytes = malloc(out->size);
		if (!out->bytes)
			return -ENOMEM;
		memcpy(out->bytes, sqlite3_column_blob(stmt, offset + 2), out->size);
	}
	return 0;
}

static int checkpoint_state_bind(sqlite3_stmt *stmt, int index,
				 const struct checkpoint_state *state)
{
	if (state->size > INT_MAX)
		return -EFBIG;
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
		return -EINVAL;
	if (path[0] == '/') {
		/* Resolve only the workspace prefix. Resolving the whole candidate
		 * would hide symlinks below the workspace from the fd walk. */
		const char *relative = !strcmp(root, "/") ? path + 1 : NULL;
		size_t path_length = strlen(path);
		for (size_t length = 1; !relative && length <= path_length; length++) {
			if (path[length] && path[length] != '/')
				continue;
			char *prefix MORPH_CLEANUP(morph_charp_cleanup) = strndup(path, length);
			char *resolved MORPH_CLEANUP(morph_charp_cleanup) =
				prefix ? realpath(prefix, NULL) : NULL;
			if (!prefix)
				return -ENOMEM;
			if (resolved && !strcmp(resolved, root)) {
				relative = path + length;
				break;
			}
		}
		if (!relative)
			return -EACCES;
		path = relative;
	}
	char *copy MORPH_CLEANUP(morph_charp_cleanup) = strdup(path);
	morph_buf_t normalized MORPH_CLEANUP(morph_buf_cleanup) = {0};
	if (!copy)
		return -ENOMEM;
	int rc = morph_buf_init(&normalized, strlen(path) + 1);
	char *save = NULL;
	for (char *part = strtok_r(copy, "/", &save); rc == 0 && part;
	     part = strtok_r(NULL, "/", &save)) {
		if (!strcmp(part, "."))
			continue;
		if (!strcmp(part, ".."))
			return -EACCES;
		if (normalized.len)
			rc = morph_buf_putc(&normalized, '/');
		if (rc == 0)
			rc = morph_buf_puts(&normalized, part);
	}
	if (rc != 0)
		return rc;
	if (!normalized.len || normalized.len >= PATH_MAX)
		return normalized.len ? -ENAMETOOLONG : -EINVAL;
	*out = morph_buf_detach(&normalized);
	return *out ? 0 : -ENOMEM;
}

static int checkpoint_walk(const char *root, const char *relative, int *parent, char **leaf)
{
	char *full MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	morph_buf_t path MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int fd MORPH_CLEANUP(checkpoint_fd_cleanup) = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	int rc = morph_buf_init(&path, strlen(root) + strlen(relative) + 2);
	if (rc == 0)
		rc = morph_buf_printf(&path, "%s/%s", root, relative);
	if (rc != 0)
		return rc;
	full = morph_buf_detach(&path);
	if (!full)
		return -ENOMEM;
	char *save = NULL;
	char *part = strtok_r(full, "/", &save);
	while (part) {
		char *next = strtok_r(NULL, "/", &save);
		if (!next) {
			*leaf = strdup(part);
			if (!*leaf)
				return -ENOMEM;
			*parent = fd;
			fd = -1;
			return 0;
		}
		int child = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (child < 0)
			return -errno;
		close(fd);
		fd = child;
		part = next;
	}
	return -EINVAL;
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
		return errno == ENOENT ? 0 : -errno;
	if (!S_ISREG(before.st_mode) || before.st_nlink != 1)
		return -ELOOP;
	if (before.st_size < 0 || (uint64_t)before.st_size > CHECKPOINT_FILE_MAX_BYTES ||
	    (uint64_t)before.st_size > limit)
		return -EFBIG;
	int fd MORPH_CLEANUP(checkpoint_fd_cleanup) =
		openat(parent, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	struct stat opened;
	if (fstat(fd, &opened) != 0)
		return -errno;
	if (!checkpoint_same_stat(&before, &opened) || !S_ISREG(opened.st_mode) ||
	    opened.st_nlink != 1)
		return -EAGAIN;
	out->exists = 1;
	out->mode = opened.st_mode & 07777;
	out->size = (size_t)before.st_size;
	if (out->size) {
		out->bytes = malloc(out->size);
		if (!out->bytes)
			return -ENOMEM;
	}
	size_t read_bytes = 0;
	while (read_bytes < out->size) {
		ssize_t got = read(fd, out->bytes + read_bytes, out->size - read_bytes);
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0)
			return -errno;
		if (got == 0)
			return -EAGAIN;
		read_bytes += (size_t)got;
	}
	unsigned char extra;
	ssize_t extra_count;
	do {
		extra_count = read(fd, &extra, 1);
	} while (extra_count < 0 && errno == EINTR);
	if (extra_count < 0)
		return -errno;
	if (extra_count)
		return -EAGAIN;
	struct stat after;
	if (fstat(fd, &after) != 0)
		return -errno;
	if (!checkpoint_same_stat(&after, &opened))
		return -EAGAIN;
	if (fstatat(parent, leaf, &after, AT_SYMLINK_NOFOLLOW) != 0)
		return -errno;
	if (!checkpoint_same_stat(&after, &opened))
		return -EAGAIN;
	return 0;
}

static int checkpoint_read(const char *root, const char *path, size_t limit,
			   struct checkpoint_state *out)
{
	int parent MORPH_CLEANUP(checkpoint_fd_cleanup) = -1;
	char *leaf MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	int rc = checkpoint_walk(root, path, &parent, &leaf);
	return rc == -ENOENT ? 0 : rc != 0 ? rc : checkpoint_read_at(parent, leaf, limit, out);
}

static int checkpoint_snapshot(struct db *db, int64_t id, int64_t session_id)
{
	struct checkpoint_table tables[9];
	int count = checkpoint_tables(tables, sizeof(tables) / sizeof(tables[0]));
	for (int i = 0; i < count; i++) {
		int present = checkpoint_table_exists(db, tables[i].name);
		if (present < 0)
			return present;
		if (!present)
			continue;
		morph_buf_t sql MORPH_CLEANUP(morph_buf_cleanup) = {0};
		sqlite3_stmt *stmt MORPH_CLEANUP(checkpoint_stmt_cleanup) = NULL;
		int rc = morph_buf_init(&sql, 256);
		if (rc == 0)
			rc = morph_buf_printf(
				&sql,
				"INSERT INTO checkpoint_rows_%s SELECT ?1,t.* FROM %s t WHERE %s",
				tables[i].name, tables[i].name, tables[i].filter);
		if (rc == 0)
			rc = checkpoint_prepare(db, morph_buf_cstr_borrow(&sql), &stmt);
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
	sqlite3_stmt *stmt MORPH_CLEANUP(checkpoint_stmt_cleanup) = NULL;
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
	struct checkpoint_table tables[9];
	int count = checkpoint_tables(tables, sizeof(tables) / sizeof(tables[0]));
	for (int i = 0; rc == 0 && i < count; i++) {
		int present = checkpoint_table_exists(db, tables[i].name);
		if (present < 0)
			return present;
		if (!present)
			continue;
		morph_buf_t sql MORPH_CLEANUP(morph_buf_cleanup) = {0};
		rc = morph_buf_init(&sql, 256);
		if (rc == 0)
			rc = morph_buf_printf(&sql,
					      "DELETE FROM checkpoint_rows_%s WHERE checkpoint_id "
					      "NOT IN(SELECT id FROM code_checkpoints)",
					      tables[i].name);
		if (rc == 0)
			rc = db_exec(db, morph_buf_cstr_borrow(&sql));
	}
	return rc;
}

int checkpoint_store_create(struct db *db, int64_t session_id, const char *workdir,
			    const char *turn_id, const char *prompt, int64_t *id)
{
	struct checkpoint_lock lock MORPH_CLEANUP(checkpoint_lock_cleanup) = {0};
	char *root MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	sqlite3_stmt *stmt MORPH_CLEANUP(checkpoint_stmt_cleanup) = NULL;
	if (session_id <= 0 || !workdir || !*workdir || !id)
		return -EINVAL;
	*id = 0;
	root = realpath(workdir, NULL);
	if (!root)
		return -errno;
	struct stat info;
	if (stat(root, &info) != 0)
		return -errno;
	if (!S_ISDIR(info.st_mode))
		return -ENOTDIR;
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
	struct checkpoint_lock lock MORPH_CLEANUP(checkpoint_lock_cleanup) = {0};
	struct checkpoint_info info MORPH_CLEANUP(checkpoint_info_cleanup) = {0};
	struct checkpoint_state current MORPH_CLEANUP(checkpoint_state_cleanup) = {0};
	struct checkpoint_state expected MORPH_CLEANUP(checkpoint_state_cleanup) = {0};
	char *relative MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	sqlite3_stmt *stmt MORPH_CLEANUP(checkpoint_stmt_cleanup) = NULL;
	if (id <= 0 || !path)
		return -EINVAL;
	int rc = checkpoint_enter(db, &lock);
	if (rc == 0)
		rc = checkpoint_info_read(db, 0, id, &info);
	if (rc != 0)
		return rc;
	if (!info.code_active)
		return -ESTALE;
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
					   CHECKPOINT_PLAN_MAX_BYTES ||
				   sqlite3_column_int64(stmt, 1) >= CHECKPOINT_PLAN_MAX_FILES) {
				rc = -EFBIG;
			}
		}
		checkpoint_stmt_cleanup(&stmt);
	}
	if (rc == 0 && previous && !checkpoint_state_equal(&current, &expected)) {
		if (previous == id) {
			rc = -EAGAIN;
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

int checkpoint_store_seal(struct db *db, int64_t id)
{
	struct checkpoint_lock lock MORPH_CLEANUP(checkpoint_lock_cleanup) = {0};
	struct checkpoint_info info MORPH_CLEANUP(checkpoint_info_cleanup) = {0};
	sqlite3_stmt *rows MORPH_CLEANUP(checkpoint_stmt_cleanup) = NULL;
	if (id <= 0)
		return -EINVAL;
	int rc = checkpoint_enter(db, &lock);
	if (rc == 0)
		rc = checkpoint_info_read(db, 0, id, &info);
	if (rc != 0)
		return rc;
	if (!info.code_active)
		return -ESTALE;
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
		struct checkpoint_state state MORPH_CLEANUP(checkpoint_state_cleanup) = {0};
		char *path MORPH_CLEANUP(morph_charp_cleanup) =
			strdup((const char *)sqlite3_column_text(rows, 0));
		sqlite3_stmt *stmt MORPH_CLEANUP(checkpoint_stmt_cleanup) = NULL;
		if (!path) {
			rc = -ENOMEM;
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
		rc = morph_array_init(&plan->files, 8, sizeof(struct checkpoint_file));
	if (rc != 0 || !plan->info.code_active)
		return rc;
	sqlite3_stmt *stmt MORPH_CLEANUP(checkpoint_stmt_cleanup) = NULL;
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
		for (size_t i = 0; i < plan->files.nelts; i++) {
			struct checkpoint_file *candidate = morph_array_borrow(&plan->files, i);
			if (!strcmp(candidate->path, path)) {
				file = candidate;
				break;
			}
		}
		if (!file) {
			if (plan->files.nelts >= CHECKPOINT_PLAN_MAX_FILES)
				return -E2BIG;
			file = morph_array_push_borrow(&plan->files);
			if (!file)
				return -ENOMEM;
			memset(file, 0, sizeof(*file));
			file->path = strdup(path);
			if (!file->path)
				return -ENOMEM;
			int size = sqlite3_column_bytes(stmt, 3);
			if (size < 0 || (size_t)size > CHECKPOINT_PLAN_MAX_BYTES - plan->bytes)
				return -EFBIG;
			rc = checkpoint_state_read_sql(stmt, 1, &file->before);
			if (rc != 0)
				return rc;
			plan->bytes += file->before.size;
		}
		plan->bytes -= file->after.size;
		checkpoint_state_cleanup(&file->after);
		int size = sqlite3_column_bytes(stmt, 6);
		if (size < 0 || (size_t)size > CHECKPOINT_PLAN_MAX_BYTES - plan->bytes)
			return -EFBIG;
		rc = checkpoint_state_read_sql(stmt, 4, &file->after);
		if (rc != 0)
			return rc;
		plan->bytes += file->after.size;
		file->sealed = sqlite3_column_int(stmt, 7);
		file->pending = sqlite3_column_int(stmt, 8);
	}
	if (step != SQLITE_DONE)
		return MORPH_ERR_DB;
	for (size_t i = 0; i < plan->files.nelts;) {
		struct checkpoint_file *file = morph_array_borrow(&plan->files, i);
		if (file->sealed && !file->pending &&
		    checkpoint_state_equal(&file->before, &file->after)) {
			plan->bytes -= file->before.size + file->after.size;
			free(file->path);
			checkpoint_state_cleanup(&file->before);
			checkpoint_state_cleanup(&file->after);
			memmove(file, file + 1, (plan->files.nelts - i - 1) * sizeof(*file));
			plan->files.nelts--;
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
	if (plan->files.nelts > 1)
		qsort(plan->files.elts, plan->files.nelts, sizeof(struct checkpoint_file),
		      checkpoint_file_compare);
	struct checkpoint_file *file;
	morph_array_foreach (file, &plan->files, struct checkpoint_file) {
		char *normalized MORPH_CLEANUP(morph_charp_cleanup) = NULL;
		int rc = checkpoint_relative(plan->info.root, file->path, &normalized);
		if (rc == 0 && strcmp(normalized, file->path))
			rc = -EACCES;
		if (rc == 0)
			rc = checkpoint_read(plan->info.root, file->path,
					     CHECKPOINT_PLAN_MAX_BYTES - plan->bytes,
					     &file->current);
		if (rc == -ENOMEM)
			return rc;
		if (file->current.size > CHECKPOINT_PLAN_MAX_BYTES - plan->bytes)
			return -EFBIG;
		plan->bytes += file->current.size;
		file->conflict = rc;
		if (rc == 0 && (!file->sealed || file->pending))
			file->conflict = -EINPROGRESS;
		if (rc == 0 && !file->conflict &&
		    !checkpoint_state_equal(&file->current, &file->after))
			file->conflict = -EAGAIN;
	}
	return 0;
}

static int checkpoint_conflict_count(const struct checkpoint_plan *plan)
{
	int count = 0;
	for (size_t i = 0; i < plan->files.nelts; i++) {
		const struct checkpoint_file *file = morph_array_borrow(&plan->files, i);
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
	if (error == -EINPROGRESS)
		return "An editing attempt was not sealed; its final file state is unknown.";
	if (error == -EAGAIN)
		return "The file changed outside the captured editing history.";
	if (error == -ELOOP || error == -ENOTDIR)
		return "A symlink, hardlink, or non-regular path prevents safe restoration.";
	if (error == -EFBIG)
		return "The file or restore plan exceeds the checkpoint size limit (16 MiB/file, "
		       "128 MiB/plan).";
	return morph_strerror(error);
}

static cJSON *checkpoint_report(const struct checkpoint_plan *plan)
{
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
	if (!report || !checkpoint_json_info(report, &plan->info))
		return NULL;
	cJSON *files = cJSON_AddArrayToObject(report, "files");
	cJSON *conflicts = cJSON_AddArrayToObject(report, "conflicts");
	if (!files || !conflicts ||
	    !cJSON_AddBoolToObject(report, "code_restorable",
				   plan->info.code_active && !checkpoint_conflict_count(plan)))
		return NULL;
	for (size_t i = 0; i < plan->files.nelts; i++) {
		const struct checkpoint_file *file = morph_array_borrow(&plan->files, i);
		cJSON *row MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
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
		if (morph_json_array_take(files, &row) != 0)
			return NULL;
		if (file->conflict) {
			row = cJSON_CreateObject();
			if (!row || !cJSON_AddStringToObject(row, "path", file->path) ||
			    !cJSON_AddStringToObject(row, "reason",
						     checkpoint_conflict_reason(file->conflict)) ||
			    !cJSON_AddNumberToObject(row, "error_code", file->conflict) ||
			    morph_json_array_take(conflicts, &row) != 0)
				return NULL;
		}
	}
	cJSON *value = report;
	report = NULL;
	return value;
}

int checkpoint_store_list_copy(struct db *db, int64_t session_id, cJSON **out)
{
	struct checkpoint_lock lock MORPH_CLEANUP(checkpoint_lock_cleanup) = {0};
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = NULL;
	sqlite3_stmt *stmt MORPH_CLEANUP(checkpoint_stmt_cleanup) = NULL;
	if (session_id <= 0 || !out)
		return -EINVAL;
	if (*out)
		return -EEXIST;
	int rc = checkpoint_enter(db, &lock);
	if (rc != 0)
		return rc;
	report = cJSON_CreateObject();
	cJSON *rows = report ? cJSON_AddArrayToObject(report, "checkpoints") : NULL;
	if (!rows)
		return -ENOMEM;
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
		struct checkpoint_info info MORPH_CLEANUP(checkpoint_info_cleanup) = {0};
		cJSON *row MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
		rc = checkpoint_info_read(db, session_id, sqlite3_column_int64(stmt, 0), &info);
		if (rc != 0)
			return rc;
		if (!row || !checkpoint_json_info(row, &info) ||
		    !cJSON_AddNumberToObject(row, "files", (double)sqlite3_column_int64(stmt, 1)) ||
		    !cJSON_AddBoolToObject(row, "code_restorable", info.code_active) ||
		    morph_json_array_take(rows, &row) != 0)
			return -ENOMEM;
	}
	if (step != SQLITE_DONE)
		return MORPH_ERR_DB;
	*out = report;
	report = NULL;
	return 0;
}

int checkpoint_store_preview_copy(struct db *db, int64_t session_id, int64_t id, cJSON **out)
{
	struct checkpoint_lock lock MORPH_CLEANUP(checkpoint_lock_cleanup) = {0};
	struct checkpoint_plan plan MORPH_CLEANUP(checkpoint_plan_cleanup) = {0};
	if (session_id <= 0 || id <= 0 || !out)
		return -EINVAL;
	if (*out)
		return -EEXIST;
	int rc = checkpoint_enter(db, &lock);
	if (rc == 0)
		rc = checkpoint_plan_load(db, session_id, id, &plan);
	if (rc == 0)
		rc = checkpoint_plan_check(&plan);
	if (rc != 0)
		return rc;
	*out = checkpoint_report(&plan);
	return *out ? 0 : -ENOMEM;
}

int checkpoint_store_info_copy(struct db *db, int64_t session_id, int64_t id, cJSON **out)
{
	struct checkpoint_lock lock MORPH_CLEANUP(checkpoint_lock_cleanup) = {0};
	struct checkpoint_info info MORPH_CLEANUP(checkpoint_info_cleanup) = {0};
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = NULL;
	if (!out || session_id <= 0 || id <= 0)
		return -EINVAL;
	if (*out)
		return -EEXIST;
	int rc = checkpoint_enter(db, &lock);
	if (rc == 0)
		rc = checkpoint_info_read(db, session_id, id, &info);
	if (rc != 0)
		return rc;
	report = cJSON_CreateObject();
	if (!report || !checkpoint_json_info(report, &info))
		return -ENOMEM;
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
			return -errno;
		if (!count)
			return -EIO;
		written += (size_t)count;
	}
	if (fchmod(fd, state->mode) != 0 || fsync(fd) != 0)
		return -errno;
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
	int parent MORPH_CLEANUP(checkpoint_fd_cleanup) = -1;
	int temporary MORPH_CLEANUP(checkpoint_fd_cleanup) = -1;
	char *leaf MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	struct checkpoint_state current MORPH_CLEANUP(checkpoint_state_cleanup) = {0};
	int moved = 0;
	int rc = checkpoint_walk(root, path, &parent, &leaf);
	if (rc == -ENOENT && !expected->exists && !desired->exists)
		return 0;
	if (rc != 0)
		return rc;
	rc = checkpoint_read_at(parent, leaf, CHECKPOINT_FILE_MAX_BYTES, &current);
	if (rc != 0)
		return rc;
	if (!checkpoint_state_equal(&current, expected))
		return -EAGAIN;
	if (checkpoint_state_equal(expected, desired))
		return 0;
	if (desired->exists) {
		temporary = openat(parent, staging,
				   O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (temporary < 0)
			return -errno;
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
			rc = -errno;
			return checkpoint_replace_recover(parent, leaf, backup, staging,
						 0, changed, staging_owned, rc);
		}
		checkpoint_fd_cleanup(&temporary);
		if (renameat(parent, leaf, parent, backup) != 0) {
			rc = -errno;
			(void)unlinkat(parent, backup, 0);
			return checkpoint_replace_recover(parent, leaf, backup, staging,
						 0, changed, staging_owned, rc);
		}
		moved = 1;
		*changed = 1;
		checkpoint_state_cleanup(&current);
		rc = checkpoint_read_at(parent, backup, CHECKPOINT_FILE_MAX_BYTES, &current);
		if (rc == 0 && !checkpoint_state_equal(&current, expected))
			rc = -EAGAIN;
		if (rc != 0)
			return checkpoint_replace_recover(parent, leaf, backup, staging,
						 moved, changed, staging_owned, rc);
	}
	if (desired->exists && linkat(parent, staging, parent, leaf, 0) != 0) {
		rc = -errno;
		return checkpoint_replace_recover(parent, leaf, backup, staging,
						 moved, changed, staging_owned, rc);
	}
	*changed = 1;
	if (*staging_owned) {
		if (unlinkat(parent, staging, 0) != 0)
			return -errno;
		*staging_owned = 0;
	}
	return 0;
}

static int checkpoint_backup_remove(const char *root, const char *path, const char *backup)
{
	if (!backup[0])
		return 0;
	int parent MORPH_CLEANUP(checkpoint_fd_cleanup) = -1;
	char *leaf MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	int rc = checkpoint_walk(root, path, &parent, &leaf);
	if (rc != 0)
		return rc;
	if (unlinkat(parent, backup, 0) != 0 && errno != ENOENT)
		return -errno;
	return 0;
}

static int checkpoint_columns(struct db *db, const char *table, morph_buf_t *out)
{
	morph_buf_t sql MORPH_CLEANUP(morph_buf_cleanup) = {0};
	sqlite3_stmt *stmt MORPH_CLEANUP(checkpoint_stmt_cleanup) = NULL;
	int rc = morph_buf_init(&sql, 128);
	if (rc == 0)
		rc = morph_buf_printf(&sql, "PRAGMA table_info(%s)", table);
	if (rc == 0)
		rc = checkpoint_prepare(db, morph_buf_cstr_borrow(&sql), &stmt);
	if (rc != 0)
		return rc;
	int step;
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *column = (const char *)sqlite3_column_text(stmt, 1);
		if (out->len)
			rc = morph_buf_putc(out, ',');
		if (rc == 0)
			rc = morph_buf_putc(out, '"');
		for (const char *cursor = column; rc == 0 && *cursor; cursor++) {
			rc = morph_buf_putc(out, *cursor);
			if (rc == 0 && *cursor == '"')
				rc = morph_buf_putc(out, '"');
		}
		if (rc == 0)
			rc = morph_buf_putc(out, '"');
		if (rc != 0)
			return rc;
	}
	return step == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int checkpoint_conversation_restore(struct db *db, const struct checkpoint_info *info)
{
	struct checkpoint_table tables[9];
	int count = checkpoint_tables(tables, sizeof(tables) / sizeof(tables[0]));
	for (int i = 0; i < count; i++) {
		int present = checkpoint_table_exists(db, tables[i].name);
		if (present < 0)
			return present;
		if (!present)
			continue;
		morph_buf_t sql MORPH_CLEANUP(morph_buf_cleanup) = {0};
		sqlite3_stmt *stmt MORPH_CLEANUP(checkpoint_stmt_cleanup) = NULL;
		int rc = morph_buf_init(&sql, 256);
		if (rc == 0)
			rc = morph_buf_printf(&sql, "DELETE FROM %s WHERE %s", tables[i].name,
					      tables[i].filter);
		if (rc == 0)
			rc = checkpoint_prepare(db, morph_buf_cstr_borrow(&sql), &stmt);
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
		morph_buf_t columns MORPH_CLEANUP(morph_buf_cleanup) = {0};
		morph_buf_t sql MORPH_CLEANUP(morph_buf_cleanup) = {0};
		sqlite3_stmt *stmt MORPH_CLEANUP(checkpoint_stmt_cleanup) = NULL;
		int rc = morph_buf_init(&columns, 128);
		if (rc == 0)
			rc = checkpoint_columns(db, tables[i].name, &columns);
		if (rc == 0)
			rc = morph_buf_init(&sql, 256);
		if (rc == 0)
			rc = morph_buf_printf(&sql,
					      "INSERT INTO %s(%s) SELECT %s FROM "
					      "checkpoint_rows_%s WHERE checkpoint_id=?",
					      tables[i].name, morph_buf_cstr_borrow(&columns),
					      morph_buf_cstr_borrow(&columns), tables[i].name);
		if (rc == 0)
			rc = checkpoint_prepare(db, morph_buf_cstr_borrow(&sql), &stmt);
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
	sqlite3_stmt *stmt MORPH_CLEANUP(checkpoint_stmt_cleanup) = NULL;
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
	cJSON *effects = cJSON_AddArrayToObject(report, "effects");
	if (!effects || !cJSON_AddBoolToObject(report, "code_restored", 0) ||
	    !cJSON_AddBoolToObject(report, "conversation_restored", 0) ||
	    !cJSON_AddBoolToObject(report, "compensated", 0) ||
	    !cJSON_AddBoolToObject(report, "rollback_incomplete", 0) ||
	    !cJSON_AddBoolToObject(report, "database_rollback_failed", 0) ||
	    !cJSON_AddBoolToObject(report, "cleanup_incomplete", 0) ||
	    !cJSON_AddNumberToObject(report, "error_code", 0))
		return -ENOMEM;
	struct checkpoint_file *file;
	morph_array_foreach (file, &plan->files, struct checkpoint_file) {
		int rc = morph_random_id(".morph-rewind-save-", file->backup, sizeof(file->backup));
		if (rc == 0)
			rc = morph_random_id(".morph-rewind-undo-", file->compensation_backup,
					     sizeof(file->compensation_backup));
		if (rc == 0)
			rc = morph_random_id(".morph-rewind-new-", file->staging,
					     sizeof(file->staging));
		if (rc == 0)
			rc = morph_random_id(".morph-rewind-comp-", file->compensation_staging,
					     sizeof(file->compensation_staging));
		if (rc != 0)
			return rc;
		morph_buf_t backup_path MORPH_CLEANUP(morph_buf_cleanup) = {0};
		rc = morph_buf_init(&backup_path, strlen(file->path) + sizeof(file->backup));
		const char *slash = strrchr(file->path, '/');
		if (rc == 0 && slash)
			rc = morph_buf_append(&backup_path, file->path,
					      (size_t)(slash - file->path) + 1);
		if (rc == 0)
			rc = morph_buf_puts(&backup_path, file->backup);
		if (rc != 0)
			return rc;
		cJSON *row MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
		if (!row || !cJSON_AddStringToObject(row, "path", file->path) ||
		    !cJSON_AddStringToObject(row, "recovery_path",
					     morph_buf_cstr_borrow(&backup_path)) ||
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
		    morph_json_array_take(effects, &row) != 0)
			return -ENOMEM;
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
	for (size_t i = plan->files.nelts; i > 0; i--) {
		struct checkpoint_file *file = morph_array_borrow(&plan->files, i - 1);
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
	struct checkpoint_lock lock MORPH_CLEANUP(checkpoint_lock_cleanup) = {0};
	struct checkpoint_plan plan MORPH_CLEANUP(checkpoint_plan_cleanup) = {0};
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = NULL;
	if (!out || id <= 0 || session_id <= 0 || mode < CHECKPOINT_RESTORE_CODE ||
	    mode > CHECKPOINT_RESTORE_BOTH)
		return -EINVAL;
	if (*out)
		return -EEXIST;
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
		return -ENOMEM;
	rc = checkpoint_effects_prepare(&plan, report);
	if (rc != 0)
		return rc;
	if (((mode & CHECKPOINT_RESTORE_CODE) && !plan.info.code_active) ||
	    ((mode & CHECKPOINT_RESTORE_CONVERSATION) && !plan.info.conversation_active)) {
		rc = -ESTALE;
	} else if ((mode & CHECKPOINT_RESTORE_CODE) && checkpoint_conflict_count(&plan)) {
		rc = -EAGAIN;
	} else {
		rc = db_exec(db, "SAVEPOINT morph_checkpoint");
		if (rc == 0) {
			if (mode & CHECKPOINT_RESTORE_CONVERSATION)
				rc = checkpoint_conversation_restore(db, &plan.info);
			cJSON *effects = cJSON_GetObjectItemCaseSensitive(report, "effects");
			for (size_t i = 0;
			     rc == 0 && (mode & CHECKPOINT_RESTORE_CODE) && i < plan.files.nelts;
			     i++) {
				struct checkpoint_file *file = morph_array_borrow(&plan.files, i);
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
					for (size_t i = 0; i < plan.files.nelts; i++) {
						struct checkpoint_file *file =
							morph_array_borrow(&plan.files, i);
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
					rc = -EIO;
				}
			}
		}
	}
	cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(report, "error_code"), rc);
	*out = report;
	report = NULL;
	return rc;
}
