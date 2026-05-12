#include "session.h"
#include "util/log.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int session_create(struct db *db, const char *name, const char *model,
		   struct session *out)
{
	if (!db || !db->handle || !name)
		return -EINVAL;
	int64_t now = (int64_t)time(NULL);
	sqlite3_stmt *stmt;
	const char *sql = "INSERT INTO sessions(name,model,created_at,updated_at,token_used)"
			  " VALUES(?,?,?,?,0)";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return -EIO;
	sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, model ? model : "", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, now);
	sqlite3_bind_int64(stmt, 4, now);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE) {
		log_err("session create failed: %s", sqlite3_errmsg(db->handle));
		return -EIO;
	}
	if (out) {
		memset(out, 0, sizeof(*out));
		out->id = sqlite3_last_insert_rowid(db->handle);
		strncpy(out->name, name, sizeof(out->name) - 1);
		if (model)
			strncpy(out->model, model, sizeof(out->model) - 1);
		out->created_at = now;
		out->updated_at = now;
	}
	log_info("session created: %s (id=%lld)", name, (long long)out->id);
	return 0;
}

int session_get_by_name(struct db *db, const char *name, struct session *out)
{
	if (!db || !db->handle || !name || !out)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql = "SELECT id,name,model,created_at,updated_at,token_used"
			  " FROM sessions WHERE name=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return -EIO;
	sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		memset(out, 0, sizeof(*out));
		out->id = sqlite3_column_int64(stmt, 0);
		strncpy(out->name, (const char *)sqlite3_column_text(stmt, 1),
			sizeof(out->name) - 1);
		const char *m = (const char *)sqlite3_column_text(stmt, 2);
		if (m)
			strncpy(out->model, m, sizeof(out->model) - 1);
		out->created_at = sqlite3_column_int64(stmt, 3);
		out->updated_at = sqlite3_column_int64(stmt, 4);
		out->token_used = sqlite3_column_int64(stmt, 5);
		sqlite3_finalize(stmt);
		return 0;
	}
	sqlite3_finalize(stmt);
	return -ENOENT;
}

int session_get_by_id(struct db *db, int64_t id, struct session *out)
{
	if (!db || !db->handle || !out)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql = "SELECT id,name,model,created_at,updated_at,token_used"
			  " FROM sessions WHERE id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return -EIO;
	sqlite3_bind_int64(stmt, 1, id);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		memset(out, 0, sizeof(*out));
		out->id = sqlite3_column_int64(stmt, 0);
		strncpy(out->name, (const char *)sqlite3_column_text(stmt, 1),
			sizeof(out->name) - 1);
		const char *m = (const char *)sqlite3_column_text(stmt, 2);
		if (m)
			strncpy(out->model, m, sizeof(out->model) - 1);
		out->created_at = sqlite3_column_int64(stmt, 3);
		out->updated_at = sqlite3_column_int64(stmt, 4);
		out->token_used = sqlite3_column_int64(stmt, 5);
		sqlite3_finalize(stmt);
		return 0;
	}
	sqlite3_finalize(stmt);
	return -ENOENT;
}

int session_list(struct db *db, struct session **out, int *count)
{
	if (!db || !db->handle || !out || !count)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql = "SELECT id,name,model,created_at,updated_at,token_used"
			  " FROM sessions ORDER BY updated_at DESC";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return -EIO;
	int cap = 16;
	int n = 0;
	struct session *list = malloc(sizeof(*list) * (size_t)cap);
	if (!list) {
		sqlite3_finalize(stmt);
		return -ENOMEM;
	}
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (n >= cap) {
			cap *= 2;
			struct session *tmp = realloc(list, sizeof(*list) * (size_t)cap);
			if (!tmp) {
				free(list);
				sqlite3_finalize(stmt);
				return -ENOMEM;
			}
			list = tmp;
		}
		memset(&list[n], 0, sizeof(list[n]));
		list[n].id = sqlite3_column_int64(stmt, 0);
		strncpy(list[n].name, (const char *)sqlite3_column_text(stmt, 1),
			sizeof(list[n].name) - 1);
		const char *m = (const char *)sqlite3_column_text(stmt, 2);
		if (m)
			strncpy(list[n].model, m, sizeof(list[n].model) - 1);
		list[n].created_at = sqlite3_column_int64(stmt, 3);
		list[n].updated_at = sqlite3_column_int64(stmt, 4);
		list[n].token_used = sqlite3_column_int64(stmt, 5);
		n++;
	}
	sqlite3_finalize(stmt);
	*out = list;
	*count = n;
	return 0;
}

int session_rename(struct db *db, int64_t id, const char *new_name)
{
	if (!db || !db->handle || !new_name)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql = "UPDATE sessions SET name=?, updated_at=? WHERE id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return -EIO;
	sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, (int64_t)time(NULL));
	sqlite3_bind_int64(stmt, 3, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return -EIO;
	return 0;
}

int session_delete(struct db *db, int64_t id)
{
	if (!db || !db->handle)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql = "DELETE FROM sessions WHERE id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return -EIO;
	sqlite3_bind_int64(stmt, 1, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return -EIO;
	return 0;
}

int session_update_model(struct db *db, int64_t id, const char *model)
{
	if (!db || !db->handle || !model)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql = "UPDATE sessions SET model=?, updated_at=? WHERE id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return -EIO;
	sqlite3_bind_text(stmt, 1, model, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, (int64_t)time(NULL));
	sqlite3_bind_int64(stmt, 3, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return -EIO;
	return 0;
}

int message_add(struct db *db, int64_t session_id, const char *role,
		const char *content, int token_count)
{
	if (!db || !db->handle || !role || !content)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql = "INSERT INTO messages(session_id,role,content,token_count,compressed,created_at)"
			  " VALUES(?,?,?,?,0,?)";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return -EIO;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, role, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, content, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 4, token_count);
	sqlite3_bind_int64(stmt, 5, (int64_t)time(NULL));
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return -EIO;
	return 0;
}

struct message *message_list(struct db *db, int64_t session_id, int *count)
{
	if (!db || !db->handle || !count)
		return NULL;
	sqlite3_stmt *stmt;
	const char *sql = "SELECT id,session_id,role,content,token_count,compressed,created_at"
			  " FROM messages WHERE session_id=? ORDER BY created_at ASC";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		*count = 0;
		return NULL;
	}
	sqlite3_bind_int64(stmt, 1, session_id);
	struct message head = {0};
	struct message *cur = &head;
	int n = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		struct message *m = calloc(1, sizeof(*m));
		if (!m)
			break;
		m->id = sqlite3_column_int64(stmt, 0);
		m->session_id = sqlite3_column_int64(stmt, 1);
		strncpy(m->role, (const char *)sqlite3_column_text(stmt, 2),
			sizeof(m->role) - 1);
		m->content = strdup((const char *)sqlite3_column_text(stmt, 3));
		m->token_count = sqlite3_column_int(stmt, 4);
		m->compressed = sqlite3_column_int(stmt, 5);
		m->created_at = sqlite3_column_int64(stmt, 6);
		cur->next = m;
		cur = m;
		n++;
	}
	sqlite3_finalize(stmt);
	*count = n;
	return head.next;
}

void message_free_list(struct message *head)
{
	struct message *cur = head;
	while (cur) {
		struct message *next = cur->next;
		free(cur->content);
		free(cur);
		cur = next;
	}
}

int message_count(struct db *db, int64_t session_id)
{
	if (!db || !db->handle)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql = "SELECT COUNT(*) FROM messages WHERE session_id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return -EIO;
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	int count = 0;
	if (rc == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}