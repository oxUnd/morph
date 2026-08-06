#include "session.h"
#include "util/array.h"
#include "util/id.h"
#include "util/log.h"
#include "util/error.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SESSION_DISPLAY_ID_RANDOM_BYTES 4

static int generate_display_id(char *buf, size_t size)
{
	return morph_random_id_nbytes("", SESSION_DISPLAY_ID_RANDOM_BYTES,
				      buf, size);
}

int session_create(struct db *db, const char *name, const char *model,
		   struct session *out)
{
	if (!db || !db->handle || !name)
		return -EINVAL;
	int64_t now = (int64_t)time(NULL);
	char display_id[16];
	int id_rc = generate_display_id(display_id, sizeof(display_id));
	if (id_rc < 0)
		return id_rc;

	char name_buf[256];
	char model_buf[64];
	name_buf[0] = '\0';
	model_buf[0] = '\0';
	strncpy(name_buf, name, sizeof(name_buf) - 1);
	name_buf[sizeof(name_buf) - 1] = '\0';
	if (model) {
		strncpy(model_buf, model, sizeof(model_buf) - 1);
		model_buf[sizeof(model_buf) - 1] = '\0';
	}

	sqlite3_stmt *stmt;
	const char *sql = "INSERT INTO sessions(display_id,name,model,created_at,updated_at,token_used)"
			  " VALUES(?,?,?,?,?,0)";
	int rc;
	int retries = 3;
	do {
		if (retries < 3) {
			id_rc = generate_display_id(display_id,
						    sizeof(display_id));
			if (id_rc < 0)
				return id_rc;
		}
		rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
		if (rc != SQLITE_OK)
			MORPH_RETURN(MORPH_ERR_DB);
		sqlite3_bind_text(stmt, 1, display_id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 2, name_buf, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, model_buf[0] ? model_buf : "", -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 4, now);
		sqlite3_bind_int64(stmt, 5, now);
		rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	} while (rc == SQLITE_CONSTRAINT && retries-- > 0);

	if (rc != SQLITE_DONE) {
		if (rc == SQLITE_CONSTRAINT) {
			log_dbg("session already exists: %s", name);
			return -EEXIST;
		}
		log_err("session create failed: %s", sqlite3_errmsg(db->handle));
		MORPH_RETURN(MORPH_ERR_DB);
	}
	if (out) {
		memset(out, 0, sizeof(*out));
		out->id = sqlite3_last_insert_rowid(db->handle);
		memcpy(out->display_id, display_id, sizeof(out->display_id));
		memcpy(out->name, name_buf, sizeof(out->name));
		memcpy(out->model, model_buf, sizeof(out->model));
		out->created_at = now;
		out->updated_at = now;
	}
	log_info("session created: %s (id=%lld, display=%s)",
		 out ? out->name : name,
		 out ? (long long)out->id :
		       (long long)sqlite3_last_insert_rowid(db->handle),
		 display_id);
	return 0;
}

int session_get_by_name(struct db *db, const char *name, struct session *out)
{
	if (!db || !db->handle || !name || !out)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql = "SELECT id,display_id,name,model,created_at,updated_at,token_used"
			  " FROM sessions WHERE name=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		memset(out, 0, sizeof(*out));
		out->id = sqlite3_column_int64(stmt, 0);
		const char *d = (const char *)sqlite3_column_text(stmt, 1);
		if (d) strncpy(out->display_id, d, sizeof(out->display_id) - 1);
		strncpy(out->name, (const char *)sqlite3_column_text(stmt, 2),
			sizeof(out->name) - 1);
		const char *m = (const char *)sqlite3_column_text(stmt, 3);
		if (m)
			strncpy(out->model, m, sizeof(out->model) - 1);
		out->created_at = sqlite3_column_int64(stmt, 4);
		out->updated_at = sqlite3_column_int64(stmt, 5);
		out->token_used = sqlite3_column_int64(stmt, 6);
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
	const char *sql = "SELECT id,display_id,name,model,created_at,updated_at,token_used"
			  " FROM sessions WHERE id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, id);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		memset(out, 0, sizeof(*out));
		out->id = sqlite3_column_int64(stmt, 0);
		const char *d = (const char *)sqlite3_column_text(stmt, 1);
		if (d) strncpy(out->display_id, d, sizeof(out->display_id) - 1);
		strncpy(out->name, (const char *)sqlite3_column_text(stmt, 2),
			sizeof(out->name) - 1);
		const char *m = (const char *)sqlite3_column_text(stmt, 3);
		if (m)
			strncpy(out->model, m, sizeof(out->model) - 1);
		out->created_at = sqlite3_column_int64(stmt, 4);
		out->updated_at = sqlite3_column_int64(stmt, 5);
		out->token_used = sqlite3_column_int64(stmt, 6);
		sqlite3_finalize(stmt);
		return 0;
	}
	sqlite3_finalize(stmt);
	return -ENOENT;
}

int session_get_by_display_id(struct db *db, const char *display_id, struct session *out)
{
	if (!db || !db->handle || !display_id || !out)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql = "SELECT id,display_id,name,model,created_at,updated_at,token_used"
			  " FROM sessions WHERE display_id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_text(stmt, 1, display_id, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		memset(out, 0, sizeof(*out));
		out->id = sqlite3_column_int64(stmt, 0);
		const char *d = (const char *)sqlite3_column_text(stmt, 1);
		if (d) strncpy(out->display_id, d, sizeof(out->display_id) - 1);
		strncpy(out->name, (const char *)sqlite3_column_text(stmt, 2),
			sizeof(out->name) - 1);
		const char *m = (const char *)sqlite3_column_text(stmt, 3);
		if (m)
			strncpy(out->model, m, sizeof(out->model) - 1);
		out->created_at = sqlite3_column_int64(stmt, 4);
		out->updated_at = sqlite3_column_int64(stmt, 5);
		out->token_used = sqlite3_column_int64(stmt, 6);
		sqlite3_finalize(stmt);
		return 0;
	}
	sqlite3_finalize(stmt);
	return -ENOENT;
}

int session_list(struct db *db, struct session **out, int *count,
		 int limit, const char *filter)
{
	if (!db || !db->handle || !out || !count)
		return -EINVAL;

	char sql[512];
	int pos = snprintf(sql, sizeof(sql),
		"SELECT id,display_id,name,model,created_at,updated_at,token_used"
		" FROM sessions WHERE NOT EXISTS (SELECT 1 FROM sub_agent_tasks "
		"WHERE sub_agent_tasks.child_session_id=sessions.id)");
	int bind_idx = 1;

	if (filter && filter[0]) {
		pos += snprintf(sql + pos, sizeof(sql) - (size_t)pos,
				" AND name LIKE ?");
	}
	pos += snprintf(sql + pos, sizeof(sql) - (size_t)pos,
			" ORDER BY updated_at DESC");
	if (limit > 0)
		pos += snprintf(sql + pos, sizeof(sql) - (size_t)pos,
				" LIMIT ?");

	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);

	if (filter && filter[0]) {
		char pattern[260];
		snprintf(pattern, sizeof(pattern), "%%%s%%", filter);
		sqlite3_bind_text(stmt, bind_idx, pattern, -1, SQLITE_TRANSIENT);
		bind_idx++;
	}
	if (limit > 0)
		sqlite3_bind_int(stmt, bind_idx, limit);

	morph_array_t arr;
	int rc_arr;

	rc_arr = morph_array_init(&arr, 16, sizeof(struct session));
	if (rc_arr < 0) {
		sqlite3_finalize(stmt);
		return rc_arr;
	}
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		struct session *s = morph_array_push(&arr);
		if (!s) {
			morph_array_cleanup(&arr);
			sqlite3_finalize(stmt);
			return -ENOMEM;
		}
		memset(s, 0, sizeof(*s));
		s->id = sqlite3_column_int64(stmt, 0);
		const char *d = (const char *)sqlite3_column_text(stmt, 1);
		if (d) strncpy(s->display_id, d, sizeof(s->display_id) - 1);
		strncpy(s->name, (const char *)sqlite3_column_text(stmt, 2),
			sizeof(s->name) - 1);
		const char *m = (const char *)sqlite3_column_text(stmt, 3);
		if (m)
			strncpy(s->model, m, sizeof(s->model) - 1);
		s->created_at = sqlite3_column_int64(stmt, 4);
		s->updated_at = sqlite3_column_int64(stmt, 5);
		s->token_used = sqlite3_column_int64(stmt, 6);
	}
	sqlite3_finalize(stmt);
	if (arr.nelts > INT_MAX) {
		morph_array_cleanup(&arr);
		return -EOVERFLOW;
	}
	*out = arr.elts;
	*count = (int)arr.nelts;
	return 0;
}

int session_count(struct db *db)
{
	if (!db || !db->handle)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql = "SELECT COUNT(*) FROM sessions";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	rc = sqlite3_step(stmt);
	int count = 0;
	if (rc == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

int session_rename(struct db *db, int64_t id, const char *new_name)
{
	if (!db || !db->handle || !new_name)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql = "UPDATE sessions SET name=?, updated_at=? WHERE id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, (int64_t)time(NULL));
	sqlite3_bind_int64(stmt, 3, id);
	rc = sqlite3_step(stmt);
	int changed = sqlite3_changes(db->handle);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	if (changed <= 0)
		MORPH_RETURN(-ENOENT);
	return 0;
}

int session_delete(struct db *db, int64_t id)
{
	if (!db || !db->handle)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql =
		"DELETE FROM sessions WHERE id=? OR id IN ("
		"SELECT child_session_id FROM sub_agent_tasks "
		"WHERE parent_session_id=?)";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
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
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_text(stmt, 1, model, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, (int64_t)time(NULL));
	sqlite3_bind_int64(stmt, 3, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	return 0;
}

int session_update_tokens(struct db *db, int64_t id, int64_t added_tokens)
{
	if (!db || !db->handle)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql = "UPDATE sessions SET token_used = token_used + ?, updated_at = ? WHERE id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, added_tokens);
	sqlite3_bind_int64(stmt, 2, (int64_t)time(NULL));
	sqlite3_bind_int64(stmt, 3, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	return 0;
}

int message_add(struct db *db, int64_t session_id, const char *role,
		const char *content, int token_count)
{
	return message_add_with_turn_id(db, session_id, role, content,
					token_count, NULL);
}

int message_add_with_turn_id(struct db *db, int64_t session_id,
			     const char *role, const char *content,
			     int token_count, const char *turn_id)
{
	if (!db || !db->handle || !role || !content)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql =
		"INSERT INTO messages(session_id,role,content,turn_id,"
		"token_count,compressed,created_at) "
		"SELECT ?,?,?,?,?,0,? WHERE ? IS NULL OR NOT EXISTS ("
		"SELECT 1 FROM messages WHERE session_id=? AND turn_id=? "
		"AND role=?)";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, role, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, content, -1, SQLITE_TRANSIENT);
	if (turn_id && turn_id[0])
		sqlite3_bind_text(stmt, 4, turn_id, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 4);
	sqlite3_bind_int(stmt, 5, token_count);
	sqlite3_bind_int64(stmt, 6, (int64_t)time(NULL));
	if (turn_id) {
		sqlite3_bind_text(stmt, 7, turn_id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 9, turn_id, -1, SQLITE_TRANSIENT);
	} else {
		sqlite3_bind_null(stmt, 7);
		sqlite3_bind_null(stmt, 9);
	}
	sqlite3_bind_int64(stmt, 8, session_id);
	sqlite3_bind_text(stmt, 10, role, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	return 0;
}

int message_delete(struct db *db, int64_t message_id)
{
	if (!db || !db->handle)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql = "DELETE FROM messages WHERE id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, message_id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return (rc == SQLITE_DONE) ? 0 : MORPH_ERR_DB;
}

struct message *message_list(struct db *db, int64_t session_id, int *count)
{
	if (!db || !db->handle || !count)
		return NULL;
	sqlite3_stmt *stmt;
	const char *sql = "SELECT id,session_id,role,content,turn_id,token_count,compressed,created_at"
			  " FROM messages WHERE session_id=?"
			  " ORDER BY created_at ASC, id ASC";
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
		{ const char *_ct = (const char *)sqlite3_column_text(stmt, 3); m->content = _ct ? strdup(_ct) : strdup(""); }
		{ const char *_tid = (const char *)sqlite3_column_text(stmt, 4); m->turn_id = _tid ? strdup(_tid) : NULL; }
		m->token_count = sqlite3_column_int(stmt, 5);
		m->compressed = sqlite3_column_int(stmt, 6);
		m->created_at = sqlite3_column_int64(stmt, 7);
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
		free(cur->turn_id);
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
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	int count = 0;
	if (rc == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

int model_history_add(struct db *db,
		      const struct model_history_insert *item,
		      int64_t *out_id)
{
	const char *sql =
		"INSERT OR IGNORE INTO model_history_items("
		"session_id,sequence_no,turn_id,kind,role,content,payload_json,"
		"tool_call_id,provider_call_id,tool_name,idempotency_key,"
		"token_count,truncated,"
		"active,created_at) "
		"SELECT ?,COALESCE(MAX(sequence_no),0)+1,?,?,?,?,?,?,?,?,?,?,?,?,? "
		"FROM model_history_items WHERE session_id=?";
	sqlite3_stmt *stmt;
	int rc;

	if (!db || !db->handle || !item || item->session_id <= 0 ||
	    !item->kind || !item->kind[0])
		MORPH_RETURN(-EINVAL);
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, item->session_id);
	if (item->turn_id)
		sqlite3_bind_text(stmt, 2, item->turn_id, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 2);
	sqlite3_bind_text(stmt, 3, item->kind, -1, SQLITE_TRANSIENT);
	if (item->role)
		sqlite3_bind_text(stmt, 4, item->role, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 4);
	if (item->content)
		sqlite3_bind_text(stmt, 5, item->content, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 5);
	if (item->payload_json)
		sqlite3_bind_text(stmt, 6, item->payload_json, -1,
				  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 6);
	if (item->tool_call_id)
		sqlite3_bind_text(stmt, 7, item->tool_call_id, -1,
				  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 7);
	if (item->provider_call_id)
		sqlite3_bind_text(stmt, 8, item->provider_call_id, -1,
				  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 8);
	if (item->tool_name)
		sqlite3_bind_text(stmt, 9, item->tool_name, -1,
				  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 9);
	if (item->idempotency_key)
		sqlite3_bind_text(stmt, 10, item->idempotency_key, -1,
				  SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 10);
	sqlite3_bind_int(stmt, 11, item->token_count);
	sqlite3_bind_int(stmt, 12, item->truncated ? 1 : 0);
	sqlite3_bind_int(stmt, 13, item->active ? 1 : 0);
	sqlite3_bind_int64(stmt, 14, (int64_t)time(NULL));
	sqlite3_bind_int64(stmt, 15, item->session_id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	if (sqlite3_changes(db->handle) == 0 && !item->idempotency_key)
		MORPH_RETURN(MORPH_ERR_DB);
	if (sqlite3_changes(db->handle) > 0) {
		if (out_id)
			*out_id = sqlite3_last_insert_rowid(db->handle);
	} else if (item->idempotency_key) {
		const char *lookup =
			"SELECT id FROM model_history_items "
			"WHERE session_id=? AND idempotency_key=?";

		rc = sqlite3_prepare_v2(db->handle, lookup, -1, &stmt, NULL);
		if (rc != SQLITE_OK)
			MORPH_RETURN(MORPH_ERR_DB);
		sqlite3_bind_int64(stmt, 1, item->session_id);
		sqlite3_bind_text(stmt, 2, item->idempotency_key, -1,
				  SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			if (out_id)
				*out_id = sqlite3_column_int64(stmt, 0);
		} else {
			rc = SQLITE_NOTFOUND;
		}
		sqlite3_finalize(stmt);
		if (rc != SQLITE_OK)
			MORPH_RETURN(MORPH_ERR_DB);
	}
	return 0;
}

static char *model_history_column_dup(sqlite3_stmt *stmt, int column)
{
	const char *value = (const char *)sqlite3_column_text(stmt, column);

	return value ? strdup(value) : NULL;
}

struct model_history_item *model_history_list(struct db *db,
					       int64_t session_id,
					       int active_only,
					       int *count)
{
	const char *sql_active =
		"SELECT id,session_id,sequence_no,turn_id,kind,role,content,"
		"payload_json,tool_call_id,provider_call_id,tool_name,"
		"idempotency_key,token_count,truncated,active,created_at "
		"FROM model_history_items "
		"WHERE session_id=? AND active=1 ORDER BY sequence_no,id";
	const char *sql_all =
		"SELECT id,session_id,sequence_no,turn_id,kind,role,content,"
		"payload_json,tool_call_id,provider_call_id,tool_name,"
		"idempotency_key,token_count,truncated,active,created_at "
		"FROM model_history_items "
		"WHERE session_id=? ORDER BY sequence_no,id";
	struct model_history_item head = {0};
	struct model_history_item *tail = &head;
	sqlite3_stmt *stmt;
	int n = 0;
	int rc;

	if (count)
		*count = 0;
	if (!db || !db->handle || session_id <= 0 || !count)
		return NULL;
	rc = sqlite3_prepare_v2(db->handle, active_only ? sql_active : sql_all,
				-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return NULL;
	sqlite3_bind_int64(stmt, 1, session_id);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		struct model_history_item *item = calloc(1, sizeof(*item));

		if (!item)
			break;
		item->id = sqlite3_column_int64(stmt, 0);
		item->session_id = sqlite3_column_int64(stmt, 1);
		item->sequence_no = sqlite3_column_int64(stmt, 2);
		item->turn_id = model_history_column_dup(stmt, 3);
		{
			const char *value =
				(const char *)sqlite3_column_text(stmt, 4);
			if (value)
				snprintf(item->kind, sizeof(item->kind), "%s", value);
			value = (const char *)sqlite3_column_text(stmt, 5);
			if (value)
				snprintf(item->role, sizeof(item->role), "%s", value);
		}
		item->content = model_history_column_dup(stmt, 6);
		item->payload_json = model_history_column_dup(stmt, 7);
		item->tool_call_id = model_history_column_dup(stmt, 8);
		item->provider_call_id = model_history_column_dup(stmt, 9);
		{
			const char *value =
				(const char *)sqlite3_column_text(stmt, 10);
			if (value)
				snprintf(item->tool_name, sizeof(item->tool_name),
					 "%s", value);
		}
		item->idempotency_key = model_history_column_dup(stmt, 11);
		item->token_count = sqlite3_column_int(stmt, 12);
		item->truncated = sqlite3_column_int(stmt, 13);
		item->active = sqlite3_column_int(stmt, 14);
		item->created_at = sqlite3_column_int64(stmt, 15);
		tail->next = item;
		tail = item;
		n++;
	}
	sqlite3_finalize(stmt);
	*count = n;
	return head.next;
}

void model_history_free_list(struct model_history_item *head)
{
	while (head) {
		struct model_history_item *next = head->next;

		free(head->turn_id);
		free(head->content);
		free(head->payload_json);
		free(head->tool_call_id);
		free(head->provider_call_id);
		free(head->idempotency_key);
		free(head);
		head = next;
	}
}

int model_history_count(struct db *db, int64_t session_id, int active_only)
{
	const char *sql_active =
		"SELECT COUNT(*) FROM model_history_items "
		"WHERE session_id=? AND active=1";
	const char *sql_all =
		"SELECT COUNT(*) FROM model_history_items WHERE session_id=?";
	sqlite3_stmt *stmt;
	int count = 0;
	int rc;

	if (!db || !db->handle || session_id <= 0)
		MORPH_RETURN(-EINVAL);
	rc = sqlite3_prepare_v2(db->handle, active_only ? sql_active : sql_all,
				-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, session_id);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

int model_history_deactivate_turn(struct db *db, int64_t session_id,
				  const char *turn_id)
{
	const char *sql =
		"UPDATE model_history_items SET active=0 "
		"WHERE session_id=? AND turn_id=? AND active=1";
	sqlite3_stmt *stmt;
	int rc;

	if (!db || !db->handle || session_id <= 0 || !turn_id || !turn_id[0])
		MORPH_RETURN(-EINVAL);
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, turn_id, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	return 0;
}

int model_history_compaction_count(struct db *db, int64_t session_id)
{
	sqlite3_stmt *stmt;
	int count = 0;
	int rc;

	if (!db || !db->handle || session_id <= 0)
		MORPH_RETURN(-EINVAL);
	rc = sqlite3_prepare_v2(db->handle,
		"SELECT COUNT(*) FROM history_compactions WHERE session_id=?",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, session_id);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

int model_history_compaction_attempt_add(struct db *db, int64_t session_id,
	const char *turn_id, const char *trigger_kind, const char *status,
	int input_tokens, int output_tokens, int error_code,
	const char *error_text)
{
	const char *sql =
		"INSERT INTO history_compaction_attempts(session_id,turn_id,"
		"trigger_kind,status,input_tokens,output_tokens,error_code,"
		"error_text,created_at) VALUES(?,?,?,?,?,?,?,?,strftime('%s','now'))";
	sqlite3_stmt *stmt;
	int rc;

	if (!db || !db->handle || session_id <= 0 || !trigger_kind ||
	    !status)
		MORPH_RETURN(-EINVAL);
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, session_id);
	if (turn_id)
		sqlite3_bind_text(stmt, 2, turn_id, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 2);
	sqlite3_bind_text(stmt, 3, trigger_kind, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, status, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 5, input_tokens);
	sqlite3_bind_int(stmt, 6, output_tokens);
	sqlite3_bind_int(stmt, 7, error_code);
	if (error_text)
		sqlite3_bind_text(stmt, 8, error_text, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 8);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	return 0;
}

int model_history_migrate_messages(struct db *db, int64_t session_id)
{
	const char *sql =
		"INSERT INTO model_history_items("
		"session_id,sequence_no,turn_id,kind,role,content,idempotency_key,"
		"token_count,"
		"truncated,active,created_at) "
		"SELECT session_id,ROW_NUMBER() OVER (ORDER BY created_at,id),"
		"turn_id,CASE WHEN role='user' THEN 'user_message' "
		"ELSE 'assistant_message' END,role,content,"
		"'legacy:message:'||id,token_count,0,1,created_at "
		"FROM messages WHERE session_id=? ORDER BY created_at,id";
	sqlite3_stmt *stmt;
	int count;
	int rc;

	if (!db || !db->handle || session_id <= 0)
		MORPH_RETURN(-EINVAL);
	count = model_history_count(db, session_id, 0);
	if (count < 0)
		return count;
	if (count > 0)
		return 0;
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	return 0;
}

static int64_t *history_preserved_add(morph_array_t *preserved, int64_t id)
{
	for (size_t i = 0; i < preserved->nelts; i++) {
		int64_t *existing = morph_array_get(preserved, i);

		if (*existing == id)
			return existing;
	}
	int64_t *slot = morph_array_push(preserved);

	if (slot)
		*slot = id;
	return slot;
}

int model_history_compact(struct db *db, int64_t session_id,
			  const char *turn_id, const char *summary,
			  int summary_tokens, int user_message_tokens,
			  int input_tokens, int keep_recent_rounds,
			  const char *trigger_kind,
			  int64_t *summary_item_id)
{
	const char *select_sql =
		"SELECT id,token_count FROM model_history_items "
		"WHERE session_id=? AND active=1 AND kind='user_message' "
		"ORDER BY sequence_no DESC";
	const char *inactive_sql =
		"UPDATE model_history_items SET active=0 "
		"WHERE session_id=? AND active=1";
	const char *restore_sql =
		"UPDATE model_history_items SET active=1 WHERE id=?";
	const char *checkpoint_sql =
		"INSERT INTO history_compactions(session_id,turn_id,"
		"cutoff_sequence_no,summary_item_id,input_tokens,output_tokens,"
		"trigger_kind,created_at) VALUES(?,?,?,?,?,?,?,strftime('%s','now'))";
	morph_array_t preserved;
	struct model_history_insert insert = {0};
	char compact_key[256];
	sqlite3_stmt *stmt = NULL;
	int64_t new_id = 0;
	int64_t cutoff_sequence_no = 0;
	int used_tokens = 0;
	int rc;

	if (!db || !db->handle || session_id <= 0 || !summary || !summary[0] ||
	    summary_tokens < 0 || user_message_tokens < 0 ||
	    keep_recent_rounds < 0 || !trigger_kind ||
	    !trigger_kind[0])
		MORPH_RETURN(-EINVAL);
	rc = morph_array_init(&preserved, 8, sizeof(int64_t));
	if (rc != 0)
		return rc;
	rc = db_exec(db, "BEGIN IMMEDIATE;");
	if (rc != 0) {
		morph_array_cleanup(&preserved);
		return rc;
	}
	rc = sqlite3_prepare_v2(db->handle, select_sql, -1, &stmt, NULL);
	if (rc == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, session_id);
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			int tokens = sqlite3_column_int(stmt, 1);
			int64_t *id;

			if (tokens < 0)
				tokens = 0;
			if (used_tokens + tokens > user_message_tokens)
				continue;
			id = history_preserved_add(&preserved,
				sqlite3_column_int64(stmt, 0));
			if (!id) {
				rc = SQLITE_NOMEM;
				break;
			}
			used_tokens += tokens;
		}
	}
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (rc != SQLITE_OK && rc != SQLITE_DONE)
		goto out_rollback;
	if (keep_recent_rounds > 0) {
		const char *recent_sql =
			"SELECT id FROM model_history_items WHERE session_id=? "
			"AND active=1 AND turn_id IN (SELECT turn_id FROM "
			"model_history_items WHERE session_id=? AND active=1 "
			"AND turn_id IS NOT NULL GROUP BY turn_id "
			"ORDER BY MAX(sequence_no) DESC LIMIT ?)";

		rc = sqlite3_prepare_v2(db->handle, recent_sql, -1, &stmt, NULL);
		if (rc != SQLITE_OK)
			goto out_rollback;
		sqlite3_bind_int64(stmt, 1, session_id);
		sqlite3_bind_int64(stmt, 2, session_id);
		sqlite3_bind_int(stmt, 3, keep_recent_rounds);
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			if (!history_preserved_add(&preserved,
				sqlite3_column_int64(stmt, 0))) {
				rc = SQLITE_NOMEM;
				break;
			}
		}
		sqlite3_finalize(stmt);
		stmt = NULL;
		if (rc != SQLITE_OK && rc != SQLITE_DONE)
			goto out_rollback;
	}
	rc = sqlite3_prepare_v2(db->handle,
		"SELECT COALESCE(MAX(sequence_no),0) FROM model_history_items "
		"WHERE session_id=?", -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		goto out_rollback;
	sqlite3_bind_int64(stmt, 1, session_id);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		cutoff_sequence_no = sqlite3_column_int64(stmt, 0);
	else
		rc = SQLITE_ERROR;
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (rc != SQLITE_OK)
		goto out_rollback;
	rc = sqlite3_prepare_v2(db->handle, inactive_sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		goto out_rollback;
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (rc != SQLITE_DONE)
		goto out_rollback;
	for (size_t i = 0; i < preserved.nelts; i++) {
		int64_t *id = morph_array_get(&preserved, i);

		rc = sqlite3_prepare_v2(db->handle, restore_sql, -1, &stmt, NULL);
		if (rc != SQLITE_OK)
			goto out_rollback;
		sqlite3_bind_int64(stmt, 1, *id);
		rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		stmt = NULL;
		if (rc != SQLITE_DONE)
			goto out_rollback;
	}
	insert.session_id = session_id;
	insert.turn_id = turn_id;
	insert.kind = "compaction_summary";
	insert.role = "system";
	insert.content = summary;
	snprintf(compact_key, sizeof(compact_key), "compaction:%s:%s",
		turn_id ? turn_id : "unknown", trigger_kind);
	insert.idempotency_key = compact_key;
	insert.token_count = summary_tokens;
	insert.active = 1;
	rc = model_history_add(db, &insert, &new_id);
	if (rc != 0)
		goto out_rollback;
	rc = sqlite3_prepare_v2(db->handle, checkpoint_sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		goto out_rollback;
	sqlite3_bind_int64(stmt, 1, session_id);
	if (turn_id)
		sqlite3_bind_text(stmt, 2, turn_id, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 2);
	sqlite3_bind_int64(stmt, 3, cutoff_sequence_no);
	sqlite3_bind_int64(stmt, 4, new_id);
	sqlite3_bind_int(stmt, 5, input_tokens);
	sqlite3_bind_int(stmt, 6, summary_tokens);
	sqlite3_bind_text(stmt, 7, trigger_kind, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (rc != SQLITE_DONE)
		goto out_rollback;
	rc = db_exec(db, "COMMIT;");
	morph_array_cleanup(&preserved);
	if (rc != 0)
		return rc;
	if (summary_item_id)
		*summary_item_id = new_id;
	return 0;

out_rollback:
	sqlite3_finalize(stmt);
	(void)db_exec(db, "ROLLBACK;");
	morph_array_cleanup(&preserved);
	MORPH_RETURN(MORPH_ERR_DB);
}

int trace_save(struct db *db, int64_t session_id, int round_no,
	       const char *steps_json, int aborted)
{
	if (!db || !db->handle || !steps_json)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql = "INSERT INTO react_traces(session_id,round_no,steps_json,aborted,created_at)"
			  " VALUES(?,?,?,?,?)";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_int(stmt, 2, round_no);
	sqlite3_bind_text(stmt, 3, steps_json, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 4, aborted);
	sqlite3_bind_int64(stmt, 5, (int64_t)time(NULL));
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return (rc == SQLITE_DONE) ? 0 : MORPH_ERR_DB;
}

char *trace_load_latest(struct db *db, int64_t session_id,
			int *out_round_no, int *out_aborted)
{
	if (!db || !db->handle)
		return NULL;
	sqlite3_stmt *stmt;
	const char *sql = "SELECT round_no,steps_json,aborted FROM react_traces"
			  " WHERE session_id=? ORDER BY round_no DESC LIMIT 1";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return NULL;
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	char *json = NULL;
	if (rc == SQLITE_ROW) {
		if (out_round_no)
			*out_round_no = sqlite3_column_int(stmt, 0);
		const char *text = (const char *)sqlite3_column_text(stmt, 1);
		if (text)
			json = strdup(text);
		if (out_aborted)
			*out_aborted = sqlite3_column_int(stmt, 2);
	}
	sqlite3_finalize(stmt);
	return json;
}

int session_ensure_display_id(struct db *db, struct session *s)
{
	if (!db || !db->handle || !s)
		return -EINVAL;
	if (s->display_id[0])
		return 0;
	char id_str[16];
	int id_rc = generate_display_id(id_str, sizeof(id_str));
	if (id_rc < 0)
		return id_rc;
	sqlite3_stmt *stmt;
	const char *sql = "UPDATE sessions SET display_id=? WHERE id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_text(stmt, 1, id_str, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, s->id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc == SQLITE_DONE) {
		memcpy(s->display_id, id_str, sizeof(s->display_id));
		return 0;
	}
	MORPH_RETURN(MORPH_ERR_DB);
}

int trace_get_next_round_no(struct db *db, int64_t session_id)
{
	if (!db || !db->handle)
		return 1;
	sqlite3_stmt *stmt;
	const char *sql = "SELECT COALESCE(MAX(round_no), 0) + 1 FROM react_traces"
			  " WHERE session_id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return 1;
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	int next = 1;
	if (rc == SQLITE_ROW)
		next = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return next;
}
