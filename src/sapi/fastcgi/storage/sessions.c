#include "sapi/fastcgi/storage/sessions.h"
#include "sapi/fastcgi/session_store_internal.h"
#include "cJSON.h"
#include "util/error.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include "session.h"

int store_create_session(struct session_store *s, const char *user_id, const char *name,
			 const char *model, char out_session_id[64])
{
	struct session sess;
	if (session_create(&s->db, name, model, &sess) != 0)
		return -EIO;
	if (session_ensure_display_id(&s->db, &sess) != 0)
		return -EIO;
	snprintf(out_session_id, 64, "%s", sess.display_id);

	const char *sql =
		"INSERT OR REPLACE INTO fcgi_session_owner(session_id,user_id) VALUES(?,?)";
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return -EIO;
	sqlite3_bind_text(st, 1, out_session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, user_id, -1, SQLITE_TRANSIENT);
	int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	return rc == SQLITE_DONE ? 0 : -EIO;
}

int store_session_owned_by(struct session_store *s, const char *session_id, const char *user_id)
{
	const char *sql = "SELECT 1 FROM fcgi_session_owner WHERE session_id=? AND user_id=?";
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return 0;
	sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, user_id, -1, SQLITE_TRANSIENT);
	int rc = sqlite3_step(st);
	sqlite3_finalize(st);
	return rc == SQLITE_ROW ? 1 : 0;
}

int store_list_sessions_page_json(struct session_store *s, const char *user_id, int limit,
				  int offset, char **out_json)
{
	const char *sql = "SELECT s.display_id, s.name, s.model, s.created_at, s.updated_at "
			  "FROM fcgi_session_owner o "
			  "JOIN sessions s ON s.display_id = o.session_id "
			  "WHERE o.user_id=? ORDER BY s.updated_at DESC LIMIT ? OFFSET ?";
	sqlite3_stmt *st = NULL;
	int lim = limit > 0 ? limit : -1;
	int fetch = lim > 0 ? lim + 1 : -1;
	int n = 0;
	int has_more = 0;
	int step_rc = SQLITE_DONE;

	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return -EIO;
	sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 2, fetch);
	sqlite3_bind_int(st, 3, offset > 0 ? offset : 0);

	cJSON *root = cJSON_CreateObject();
	cJSON *items = root ? cJSON_AddArrayToObject(root, "items") : NULL;
	if (!root || !items) {
		cJSON_Delete(root);
		sqlite3_finalize(st);
		return -ENOMEM;
	}
	while ((step_rc = sqlite3_step(st)) == SQLITE_ROW) {
		const char *id = (const char *)sqlite3_column_text(st, 0);
		const char *name = (const char *)sqlite3_column_text(st, 1);
		const char *model = (const char *)sqlite3_column_text(st, 2);
		int64_t ca = sqlite3_column_int64(st, 3);
		int64_t ua = sqlite3_column_int64(st, 4);
		cJSON *item = cJSON_CreateObject();

		if (lim > 0 && n >= lim) {
			has_more = 1;
			cJSON_Delete(item);
			break;
		}
		if (!item) {
			cJSON_Delete(root);
			sqlite3_finalize(st);
			return -ENOMEM;
		}
		cJSON_AddStringToObject(item, "id", id ? id : "");
		cJSON_AddStringToObject(item, "name", name ? name : "");
		cJSON_AddStringToObject(item, "model", model ? model : "");
		cJSON_AddNumberToObject(item, "created_at", (double)ca);
		cJSON_AddNumberToObject(item, "updated_at", (double)ua);
		cJSON_AddItemToArray(items, item);
		n++;
	}
	sqlite3_finalize(st);
	if (step_rc != SQLITE_DONE && !has_more) {
		cJSON_Delete(root);
		return -EIO;
	}
	cJSON_AddNumberToObject(root, "limit", lim > 0 ? lim : 0);
	cJSON_AddNumberToObject(root, "offset", offset > 0 ? offset : 0);
	cJSON_AddBoolToObject(root, "has_more", has_more);
	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json)
		return -ENOMEM;
	*out_json = json;
	return 0;
}

int store_list_sessions_json(struct session_store *s, const char *user_id, char **out_json)
{
	return store_list_sessions_page_json(s, user_id, 0, 0, out_json);
}

int store_update_session(struct session_store *s, const char *session_id, const char *user_id,
			 const char *name, const char *model)
{
	const char *sql = "UPDATE sessions SET name=COALESCE(?,name), "
			  "model=COALESCE(?,model), updated_at=? "
			  "WHERE display_id=? AND EXISTS ("
			  "SELECT 1 FROM fcgi_session_owner o "
			  "WHERE o.session_id=? AND o.user_id=?)";
	sqlite3_stmt *st = NULL;
	int rc;

	if (!name && !model)
		return -EINVAL;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return -EIO;
	if (name)
		sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(st, 1);
	if (model)
		sqlite3_bind_text(st, 2, model, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(st, 2);
	sqlite3_bind_int64(st, 3, fcgi_store_now());
	sqlite3_bind_text(st, 4, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 5, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 6, user_id, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_DONE)
		return -EIO;
	return sqlite3_changes(s->db.handle) > 0 ? 0 : -ENOENT;
}

int store_messages_json(struct session_store *s, const char *session_id, const char *user_id,
			int limit, int64_t before, char **out_json)
{
	const char *sql = "SELECT s.id FROM sessions s "
			  "JOIN fcgi_session_owner o ON o.session_id=s.display_id "
			  "WHERE s.display_id=? AND o.user_id=?";
	sqlite3_stmt *st = NULL;
	struct message *head;
	struct message *m;
	cJSON *root;
	cJSON *items;
	int total = 0;
	int skipped;
	int added = 0;
	int64_t internal_id = 0;
	char *json;

	if (limit <= 0 || limit > 500)
		limit = 100;
	if (sqlite3_prepare_v2(s->db.handle, sql, -1, &st, NULL) != SQLITE_OK)
		return -EIO;
	sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, user_id, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(st) == SQLITE_ROW)
		internal_id = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	if (internal_id == 0)
		return -ENOENT;

	head = message_list(&s->db, internal_id, &total);
	if (!head && total > 0)
		return -EIO;
	for (m = head; m; m = m->next)
		if (before <= 0 || m->id < before)
			added++;
	skipped = added > limit ? added - limit : 0;

	root = cJSON_CreateObject();
	items = root ? cJSON_AddArrayToObject(root, "items") : NULL;
	if (!root || !items) {
		cJSON_Delete(root);
		message_free_list(head);
		return -ENOMEM;
	}
	added = 0;
	for (m = head; m; m = m->next) {
		cJSON *item;

		if (before > 0 && m->id >= before)
			continue;
		if (added++ < skipped)
			continue;
		item = cJSON_CreateObject();
		if (!item) {
			cJSON_Delete(root);
			message_free_list(head);
			return -ENOMEM;
		}
		cJSON_AddNumberToObject(item, "id", (double)m->id);
		cJSON_AddStringToObject(item, "role", m->role);
		cJSON_AddStringToObject(item, "content", m->content ? m->content : "");
		if (m->turn_id)
			cJSON_AddStringToObject(item, "turn_id", m->turn_id);
		cJSON_AddNumberToObject(item, "token_count", m->token_count);
		cJSON_AddBoolToObject(item, "compressed", m->compressed != 0);
		cJSON_AddNumberToObject(item, "created_at", (double)m->created_at);
		cJSON_AddItemToArray(items, item);
	}
	message_free_list(head);
	cJSON_AddNumberToObject(root, "limit", limit);
	cJSON_AddBoolToObject(root, "has_more", skipped > 0);
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json)
		return -ENOMEM;
	*out_json = json;
	return 0;
}

int store_delete_session(struct session_store *s, const char *session_id, const char *user_id)
{
	static const char chk_owner[] = "SELECT 1 FROM fcgi_session_owner"
					" WHERE session_id=? AND user_id=?";
	static const char chk_turns[] =
		"SELECT COUNT(*) FROM fcgi_running_turns WHERE session_id=?";
	static const char *const del_tables[] = {
		"DELETE FROM fcgi_session_owner WHERE session_id=?",
		"DELETE FROM fcgi_events WHERE session_id=?",
		"DELETE FROM fcgi_actions WHERE session_id=?",
		"DELETE FROM fcgi_canvas_nodes WHERE session_id=?",
		"DELETE FROM fcgi_artifacts WHERE session_id=?",
		"DELETE FROM fcgi_running_turns WHERE session_id=?",
		NULL};
	sqlite3_stmt *st = NULL;
	struct session sess;
	int rc;

	if (!s || !session_id || !user_id)
		return -EINVAL;

	rc = store_transaction_begin(s);
	if (rc != 0)
		return rc;

	if (sqlite3_prepare_v2(s->db.handle, chk_owner, -1, &st, NULL) != SQLITE_OK)
		return store_transaction_end(s, -EIO);
	sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, user_id, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(st);
	sqlite3_finalize(st);
	if (rc != SQLITE_ROW)
		return store_transaction_end(s, rc == SQLITE_DONE ? -ENOENT : -EIO);

	if (sqlite3_prepare_v2(s->db.handle, chk_turns, -1, &st, NULL) != SQLITE_OK)
		return store_transaction_end(s, -EIO);
	sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(st);
	int running = (rc == SQLITE_ROW) ? sqlite3_column_int(st, 0) : 0;
	sqlite3_finalize(st);
	if (rc != SQLITE_ROW)
		return store_transaction_end(s, -EIO);
	if (running > 0)
		return store_transaction_end(s, -EAGAIN);

	for (int i = 0; del_tables[i]; i++) {
		if (sqlite3_prepare_v2(s->db.handle, del_tables[i], -1, &st, NULL) != SQLITE_OK)
			return store_transaction_end(s, -EIO);
		sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
		rc = sqlite3_step(st);
		sqlite3_finalize(st);
		if (rc != SQLITE_DONE)
			return store_transaction_end(s, -EIO);
	}

	if (session_get_by_display_id(&s->db, session_id, &sess) != 0)
		return store_transaction_end(s, -EIO);
	if (session_delete(&s->db, sess.id) != 0)
		return store_transaction_end(s, -EIO);

	return store_transaction_end(s, 0);
}
