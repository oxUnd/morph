#include "session.h"
#include "nb/vec.h"
#include "nb/cleanup.h"
#include "nb/buf.h"
#include "nb/random.h"
#include "nb/encoding.h"
#include "nb/log.h"
#include "errors.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SESSION_DISPLAY_ID_RANDOM_BYTES 4

static int generate_display_id(char *buf, size_t size)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;

	return ((file_status_1 =
			 ((SESSION_DISPLAY_ID_RANDOM_BYTES) == 0 ||
					  (SESSION_DISPLAY_ID_RANDOM_BYTES) > 16
				  ? NB_ERR_INVAL
			  : !(nb_buf_clear(&file_value_1), nb_buf_append_cstr(&file_value_1, ""))
				  ? NB_ERR_NOMEM
				  : nb_random_hex(&file_value_1, SESSION_DISPLAY_ID_RANDOM_BYTES,
						  NULL))) != NB_OK
			? file_status_1
		: (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), buf, size)) ==
				NB_ERR_RANGE
			? NB_ERR_NO_SPACE
			: file_status_1);
}

int session_create_with_workdir(struct db *db, const char *name, const char *model,
				const char *workdir, struct session *out)
{
	if (!db || !db->handle || !name)
		return NB_ERR_INVAL;
	int64_t now = (int64_t)time(NULL);
	char display_id[16];
	int id_rc = generate_display_id(display_id, sizeof(display_id));
	if (id_rc < 0)
		return id_rc;

	char name_buf[256];
	char model_buf[64];
	char workdir_buf[PATH_MAX];
	name_buf[0] = '\0';
	model_buf[0] = '\0';
	workdir_buf[0] = '\0';
	strncpy(name_buf, name, sizeof(name_buf) - 1);
	name_buf[sizeof(name_buf) - 1] = '\0';
	if (model) {
		strncpy(model_buf, model, sizeof(model_buf) - 1);
		model_buf[sizeof(model_buf) - 1] = '\0';
	}
	if (workdir) {
		strncpy(workdir_buf, workdir, sizeof(workdir_buf) - 1);
		workdir_buf[sizeof(workdir_buf) - 1] = '\0';
	}

	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	int lock_rc = db_lock_acquire(db, &lock);
	if (lock_rc != 0)
		return lock_rc;
	sqlite3_stmt *stmt;
	const char *sql =
		"INSERT INTO sessions(display_id,name,model,workdir,created_at,updated_at,token_used)"
		" VALUES(?,?,?,?,?,?,0)";
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
			return MORPH_ERR_DB;
		sqlite3_bind_text(stmt, 1, display_id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 2, name_buf, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, model_buf[0] ? model_buf : "", -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, workdir_buf, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 5, now);
		sqlite3_bind_int64(stmt, 6, now);
		rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	} while (rc == SQLITE_CONSTRAINT && retries-- > 0);

	if (rc != SQLITE_DONE) {
		if (rc == SQLITE_CONSTRAINT) {
			nb_log_write(db ? db->logger : NULL, NB_LOG_DEBUG,
				     "session already exists: %s", name);
			return NB_ERR_EXISTS;
		}
		nb_log_write(db ? db->logger : NULL, NB_LOG_ERROR, "session create failed: %s",
			     sqlite3_errmsg(db->handle));
		return MORPH_ERR_DB;
	}
	int64_t created_id = sqlite3_last_insert_rowid(db->handle);
	db_lock_cleanup(&lock);
	if (out) {
		memset(out, 0, sizeof(*out));
		out->id = created_id;
		memcpy(out->display_id, display_id, sizeof(out->display_id));
		memcpy(out->name, name_buf, sizeof(out->name));
		memcpy(out->model, model_buf, sizeof(out->model));
		memcpy(out->workdir, workdir_buf, sizeof(out->workdir));
		out->created_at = now;
		out->updated_at = now;
	}
	nb_log_write(db ? db->logger : NULL, NB_LOG_INFO,
		     "session created: %s (id=%lld, display=%s)", out ? out->name : name,
		     (long long)created_id, display_id);
	return 0;
}

int session_create(struct db *db, const char *name, const char *model,
		   struct session *out)
{
	return session_create_with_workdir(db, name, model, NULL, out);
}

static void session_read_row(sqlite3_stmt *stmt, struct session *session)
{
	const char *display = (const char *)sqlite3_column_text(stmt, 1);
	const char *name = (const char *)sqlite3_column_text(stmt, 2);
	const char *model = (const char *)sqlite3_column_text(stmt, 3);
	const char *workdir = (const char *)sqlite3_column_text(stmt, 7);

	memset(session, 0, sizeof(*session));
	session->id = sqlite3_column_int64(stmt, 0);
	snprintf(session->display_id, sizeof(session->display_id), "%s", display ? display : "");
	snprintf(session->name, sizeof(session->name), "%s", name ? name : "");
	snprintf(session->model, sizeof(session->model), "%s", model ? model : "");
	snprintf(session->workdir, sizeof(session->workdir), "%s", workdir ? workdir : "");
	session->created_at = sqlite3_column_int64(stmt, 4);
	session->updated_at = sqlite3_column_int64(stmt, 5);
	session->token_used = sqlite3_column_int64(stmt, 6);
}

int session_get_by_name(struct db *db, const char *name, struct session *out)
{
	if (!db || !db->handle || !name || !out)
		return NB_ERR_INVAL;
	sqlite3_stmt *stmt;
	const char *sql =
		"SELECT id,display_id,name,model,created_at,updated_at,token_used,workdir"
		" FROM sessions WHERE name=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		session_read_row(stmt, out);
		sqlite3_finalize(stmt);
		return 0;
	}
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? NB_ERR_NOT_FOUND : MORPH_ERR_DB;
}

int session_get_by_id(const struct db *db, int64_t id, struct session *out)
{
	if (!db || !db->handle || !out)
		return NB_ERR_INVAL;
	sqlite3_stmt *stmt;
	const char *sql =
		"SELECT id,display_id,name,model,created_at,updated_at,token_used,workdir"
		" FROM sessions WHERE id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, id);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		session_read_row(stmt, out);
		sqlite3_finalize(stmt);
		return 0;
	}
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? NB_ERR_NOT_FOUND : MORPH_ERR_DB;
}

int session_get_by_display_id(struct db *db, const char *display_id, struct session *out)
{
	if (!db || !db->handle || !display_id || !out)
		return NB_ERR_INVAL;
	sqlite3_stmt *stmt;
	const char *sql =
		"SELECT id,display_id,name,model,created_at,updated_at,token_used,workdir"
		" FROM sessions WHERE display_id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_text(stmt, 1, display_id, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		session_read_row(stmt, out);
		sqlite3_finalize(stmt);
		return 0;
	}
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? NB_ERR_NOT_FOUND : MORPH_ERR_DB;
}

int session_list(struct db *db, struct session **out, int *count, int limit, const char *filter)
{
	nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf pattern NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_vec sessions = {0};
	sqlite3_stmt *stmt = NULL;
	int parameter = 1;
	int step = SQLITE_DONE;
	int rc;

	if (!db || !db->handle || !out || !count)
		return NB_ERR_INVAL;
	*out = NULL;
	*count = 0;
	rc = (nb_buf_init(&sql), nb_buf_reserve(&sql, 256) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		nb_buf_append_cstr(
			&sql,
			"SELECT id,display_id,name,model,created_at,updated_at,token_used,workdir"
			" FROM sessions WHERE NOT EXISTS (SELECT 1 FROM sub_agent_tasks "
			"WHERE sub_agent_tasks.child_session_id=sessions.id) "
			"AND NOT EXISTS (SELECT 1 FROM agent_threads "
			"WHERE agent_threads.session_id=sessions.id)");
	if (rc == 0 && filter && filter[0])
		nb_buf_append_cstr(&sql, " AND name LIKE ?");
	if (rc == 0)
		nb_buf_append_cstr(&sql, " ORDER BY updated_at DESC");
	if (rc == 0 && limit > 0)
		nb_buf_append_cstr(&sql, " LIMIT ?");
	if (rc == 0)
		rc = nb_buf_status(&sql);
	if (rc == 0 &&
	    sqlite3_prepare_v2(db->handle, nb_buf_cstr(&sql), -1, &stmt, NULL) != SQLITE_OK)
		rc = MORPH_ERR_DB;
	nb_buf_cleanup(&sql);
	if (rc != 0)
		return rc;
	if (filter && filter[0]) {
		rc = (nb_buf_init(&pattern), nb_buf_reserve(&pattern, 256) ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_printf(&pattern, "%%%s%%", filter) ? 0 : NB_ERR_NOMEM);
		if (rc == 0 && sqlite3_bind_text(stmt, parameter++, nb_buf_cstr(&pattern), -1,
						 SQLITE_TRANSIENT) != SQLITE_OK)
			rc = MORPH_ERR_DB;
		nb_buf_cleanup(&pattern);
	}
	if (rc == 0 && limit > 0 && sqlite3_bind_int(stmt, parameter, limit) != SQLITE_OK)
		rc = MORPH_ERR_DB;
	if (rc == 0)
		rc = (nb_vec_init(&sessions, sizeof(struct session)),
		      nb_vec_reserve(&sessions, 16) ? 0 : NB_ERR_NOMEM);
	while (rc == 0 && (step = sqlite3_step(stmt)) == SQLITE_ROW) {
		struct session *session = nb_vec_push(&sessions);

		if (!session) {
			rc = NB_ERR_NOMEM;
			break;
		}
		session_read_row(stmt, session);
	}
	sqlite3_finalize(stmt);
	if (rc == 0 && step != SQLITE_DONE)
		rc = MORPH_ERR_DB;
	if (rc == 0 && sessions.len > INT_MAX)
		rc = NB_ERR_RANGE;
	if (rc != 0) {
		nb_vec_cleanup(&sessions);
		return rc;
	}
	*out = sessions.len ? malloc(sessions.len * sessions.elem_size) : NULL;
	if (sessions.len && !*out) {
		nb_vec_cleanup(&sessions);
		return NB_ERR_NOMEM;
	}
	if (sessions.len)
		memcpy(*out, sessions.data, sessions.len * sessions.elem_size);
	*count = (int)sessions.len;
	sessions.len = 0;
	nb_vec_cleanup(&sessions);
	return 0;
}

int session_count(struct db *db)
{
	if (!db || !db->handle)
		return NB_ERR_INVAL;
	sqlite3_stmt *stmt;
	const char *sql = "SELECT COUNT(*) FROM sessions";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
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
		return NB_ERR_INVAL;
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	int lock_rc = db_lock_acquire(db, &lock);
	if (lock_rc != 0)
		return lock_rc;
	sqlite3_stmt *stmt;
	const char *sql = "UPDATE sessions SET name=?, updated_at=? WHERE id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, (int64_t)time(NULL));
	sqlite3_bind_int64(stmt, 3, id);
	rc = sqlite3_step(stmt);
	int changed = sqlite3_changes(db->handle);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	if (changed <= 0)
		return NB_ERR_NOT_FOUND;
	return 0;
}

int session_delete(struct db *db, int64_t id)
{
	if (!db || !db->handle)
		return NB_ERR_INVAL;
	sqlite3_stmt *stmt;
	const char *sql =
		"DELETE FROM sessions WHERE id=? OR id IN ("
		"SELECT child_session_id FROM sub_agent_tasks "
		"WHERE parent_session_id=?) OR id IN ("
		"SELECT session_id FROM agent_threads WHERE root_session_id=?)";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_int64(stmt, 2, id);
	sqlite3_bind_int64(stmt, 3, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	return 0;
}

int session_update_model(struct db *db, int64_t id, const char *model)
{
	if (!db || !db->handle || !model)
		return NB_ERR_INVAL;
	sqlite3_stmt *stmt;
	const char *sql = "UPDATE sessions SET model=? WHERE id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_text(stmt, 1, model, -1, SQLITE_TRANSIENT);
	/* Model synchronization also runs when merely opening a session. */
	sqlite3_bind_int64(stmt, 2, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	return 0;
}

int session_update_workdir(struct db *db, int64_t id, const char *workdir)
{
	if (!db || !db->handle || id <= 0 || !workdir)
		return NB_ERR_INVAL;
	sqlite3_stmt *stmt;
	const char *sql = "UPDATE sessions SET workdir=? WHERE id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_text(stmt, 1, workdir, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	return 0;
}

int session_update_tokens(struct db *db, int64_t id, int64_t added_tokens)
{
	if (!db || !db->handle)
		return NB_ERR_INVAL;
	sqlite3_stmt *stmt;
	const char *sql = "UPDATE sessions SET token_used = token_used + ?, updated_at = ? WHERE id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, added_tokens);
	sqlite3_bind_int64(stmt, 2, (int64_t)time(NULL));
	sqlite3_bind_int64(stmt, 3, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
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
		return NB_ERR_INVAL;
	sqlite3_stmt *stmt;
	const char *sql =
		"INSERT INTO messages(session_id,role,content,turn_id,"
		"token_count,compressed,created_at) "
		"SELECT ?,?,?,?,?,0,? WHERE ? IS NULL OR NOT EXISTS ("
		"SELECT 1 FROM messages WHERE session_id=? AND turn_id=? "
		"AND role=?)";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
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
		return MORPH_ERR_DB;
	return 0;
}

int message_delete(struct db *db, int64_t message_id)
{
	if (!db || !db->handle)
		return NB_ERR_INVAL;
	sqlite3_stmt *stmt;
	const char *sql = "DELETE FROM messages WHERE id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
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
		return NB_ERR_INVAL;
	sqlite3_stmt *stmt;
	const char *sql = "SELECT COUNT(*) FROM messages WHERE session_id=?";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
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
		return NB_ERR_INVAL;
	sqlite3_stmt *stmt;
	const char *sql = "INSERT INTO react_traces(session_id,round_no,steps_json,aborted,created_at)"
			  " VALUES(?,?,?,?,?)";
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
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
		return NB_ERR_INVAL;
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
		return MORPH_ERR_DB;
	sqlite3_bind_text(stmt, 1, id_str, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, s->id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc == SQLITE_DONE) {
		memcpy(s->display_id, id_str, sizeof(s->display_id));
		return 0;
	}
	return MORPH_ERR_DB;
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
