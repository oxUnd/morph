#include "session.h"
#include "util/log.h"
#include "util/error.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void generate_display_id(char *buf, size_t size)
{
	unsigned char raw[4];
#ifdef __APPLE__
	arc4random_buf(raw, sizeof(raw));
#else
	static int seeded = 0;
	if (!seeded) { srand((unsigned)(time(NULL) ^ (uintptr_t)buf)); seeded = 1; }
	for (size_t i = 0; i < sizeof(raw); i++) raw[i] = (unsigned char)(rand() & 0xFF);
#endif
	snprintf(buf, size, "%08x",
		 (unsigned)(((unsigned)raw[0] << 24) | ((unsigned)raw[1] << 16)
			  | ((unsigned)raw[2] << 8) | (unsigned)raw[3]));
}

int session_create(struct db *db, const char *name, const char *model,
		   struct session *out)
{
	if (!db || !db->handle || !name)
		return -EINVAL;
	int64_t now = (int64_t)time(NULL);
	char display_id[16];
	generate_display_id(display_id, sizeof(display_id));

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
		if (retries < 3) /* regenerate on retry */
			generate_display_id(display_id, sizeof(display_id));
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

int session_list(struct db *db, struct session **out, int *count)
{
	if (!db || !db->handle || !out || !count)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql = "SELECT id,display_id,name,model,created_at,updated_at,token_used"
			  " FROM sessions ORDER BY updated_at DESC";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
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
		const char *d = (const char *)sqlite3_column_text(stmt, 1);
		if (d) strncpy(list[n].display_id, d, sizeof(list[n].display_id) - 1);
		strncpy(list[n].name, (const char *)sqlite3_column_text(stmt, 2),
			sizeof(list[n].name) - 1);
		const char *m = (const char *)sqlite3_column_text(stmt, 3);
		if (m)
			strncpy(list[n].model, m, sizeof(list[n].model) - 1);
		list[n].created_at = sqlite3_column_int64(stmt, 4);
		list[n].updated_at = sqlite3_column_int64(stmt, 5);
		list[n].token_used = sqlite3_column_int64(stmt, 6);
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
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, (int64_t)time(NULL));
	sqlite3_bind_int64(stmt, 3, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
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
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, id);
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
	if (!db || !db->handle || !role || !content)
		return -EINVAL;
	sqlite3_stmt *stmt;
	const char *sql = "INSERT INTO messages(session_id,role,content,token_count,compressed,created_at)"
			  " VALUES(?,?,?,?,0,?)";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, role, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, content, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 4, token_count);
	sqlite3_bind_int64(stmt, 5, (int64_t)time(NULL));
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
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	int count = 0;
	if (rc == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
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
	generate_display_id(id_str, sizeof(id_str));
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