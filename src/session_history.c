#include "session_history.h"
#include "session.h"
#include "util/error.h"
#include "util/cleanup.h"
#include "util/log.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void history_stmt_cleanup(sqlite3_stmt **stmt)
{
	sqlite3_finalize(*stmt);
	*stmt = NULL;
}

int model_history_add(struct db *db, const struct model_history_insert *item, int64_t *out_id)
{
	const char *sql = "INSERT OR IGNORE INTO model_history_items("
			  "session_id,sequence_no,turn_id,kind,role,content,payload_json,"
			  "tool_call_id,provider_call_id,tool_name,idempotency_key,"
			  "token_count,truncated,"
			  "active,created_at) "
			  "SELECT ?,COALESCE(MAX(sequence_no),0)+1,?,?,?,?,?,?,?,?,?,?,?,?,? "
			  "FROM model_history_items WHERE session_id=?";
	struct db_lock lock MORPH_CLEANUP(db_lock_cleanup) = {0};
	sqlite3_stmt *stmt MORPH_CLEANUP(history_stmt_cleanup) = NULL;
	int rc;

	if (!db || !db->handle || !item || item->session_id <= 0 || !item->kind || !item->kind[0])
		return -EINVAL;
	rc = db_lock_acquire(db, &lock);
	if (rc != 0)
		return rc;
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		log_err(db->logger, "history insert prepare failed (%d): %s", rc,
			sqlite3_errmsg(db->handle));
		return MORPH_ERR_DB;
	}
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
		sqlite3_bind_text(stmt, 6, item->payload_json, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 6);
	if (item->tool_call_id)
		sqlite3_bind_text(stmt, 7, item->tool_call_id, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 7);
	if (item->provider_call_id)
		sqlite3_bind_text(stmt, 8, item->provider_call_id, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 8);
	if (item->tool_name)
		sqlite3_bind_text(stmt, 9, item->tool_name, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 9);
	if (item->idempotency_key)
		sqlite3_bind_text(stmt, 10, item->idempotency_key, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(stmt, 10);
	sqlite3_bind_int(stmt, 11, item->token_count);
	sqlite3_bind_int(stmt, 12, item->truncated ? 1 : 0);
	sqlite3_bind_int(stmt, 13, item->active ? 1 : 0);
	sqlite3_bind_int64(stmt, 14, (int64_t)time(NULL));
	sqlite3_bind_int64(stmt, 15, item->session_id);
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		log_err(db->logger, "history insert failed (%d): %s", rc,
			sqlite3_errmsg(db->handle));
		return MORPH_ERR_DB;
	}
	history_stmt_cleanup(&stmt);
	if (sqlite3_changes(db->handle) == 0 && !item->idempotency_key)
		return MORPH_ERR_DB;
	if (sqlite3_changes(db->handle) > 0) {
		if (out_id)
			*out_id = sqlite3_last_insert_rowid(db->handle);
	} else if (item->idempotency_key) {
		const char *lookup = "SELECT id FROM model_history_items "
				     "WHERE session_id=? AND idempotency_key=?";

		rc = sqlite3_prepare_v2(db->handle, lookup, -1, &stmt, NULL);
		if (rc != SQLITE_OK)
			return MORPH_ERR_DB;
		sqlite3_bind_int64(stmt, 1, item->session_id);
		sqlite3_bind_text(stmt, 2, item->idempotency_key, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			if (out_id)
				*out_id = sqlite3_column_int64(stmt, 0);
		} else {
			rc = SQLITE_NOTFOUND;
		}
		history_stmt_cleanup(&stmt);
		if (rc != SQLITE_OK)
			return MORPH_ERR_DB;
	}
	return 0;
}

static char *model_history_column_dup(sqlite3_stmt *stmt, int column)
{
	const char *value = (const char *)sqlite3_column_text(stmt, column);

	return value ? strdup(value) : NULL;
}

struct model_history_item *model_history_list(struct db *db, int64_t session_id, int active_only,
					      int *count)
{
	const char *sql_active = "SELECT id,session_id,sequence_no,turn_id,kind,role,content,"
				 "payload_json,tool_call_id,provider_call_id,tool_name,"
				 "idempotency_key,token_count,truncated,active,created_at "
				 "FROM model_history_items "
				 "WHERE session_id=? AND active=1 ORDER BY sequence_no,id";
	const char *sql_all = "SELECT id,session_id,sequence_no,turn_id,kind,role,content,"
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
		*count = -1;
	if (!db || !db->handle || session_id <= 0 || !count)
		return NULL;
	rc = sqlite3_prepare_v2(db->handle, active_only ? sql_active : sql_all, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return NULL;
	sqlite3_bind_int64(stmt, 1, session_id);
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		struct model_history_item *item = calloc(1, sizeof(*item));

		if (!item) {
			rc = SQLITE_NOMEM;
			break;
		}
		item->id = sqlite3_column_int64(stmt, 0);
		item->session_id = sqlite3_column_int64(stmt, 1);
		item->sequence_no = sqlite3_column_int64(stmt, 2);
		item->turn_id = model_history_column_dup(stmt, 3);
		{
			const char *value = (const char *)sqlite3_column_text(stmt, 4);
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
			const char *value = (const char *)sqlite3_column_text(stmt, 10);
			if (value)
				snprintf(item->tool_name, sizeof(item->tool_name), "%s", value);
		}
		item->idempotency_key = model_history_column_dup(stmt, 11);
		item->token_count = sqlite3_column_int(stmt, 12);
		item->truncated = sqlite3_column_int(stmt, 13);
		item->active = sqlite3_column_int(stmt, 14);
		item->created_at = sqlite3_column_int64(stmt, 15);
		if ((sqlite3_column_type(stmt, 3) != SQLITE_NULL && !item->turn_id) ||
		    (sqlite3_column_type(stmt, 6) != SQLITE_NULL && !item->content) ||
		    (sqlite3_column_type(stmt, 7) != SQLITE_NULL && !item->payload_json) ||
		    (sqlite3_column_type(stmt, 8) != SQLITE_NULL && !item->tool_call_id) ||
		    (sqlite3_column_type(stmt, 9) != SQLITE_NULL && !item->provider_call_id) ||
		    (sqlite3_column_type(stmt, 11) != SQLITE_NULL && !item->idempotency_key)) {
			model_history_free_list(item);
			rc = SQLITE_NOMEM;
			break;
		}
		tail->next = item;
		tail = item;
		n++;
	}
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE) {
		model_history_free_list(head.next);
		return NULL;
	}
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
	const char *sql_active = "SELECT COUNT(*) FROM model_history_items "
				 "WHERE session_id=? AND active=1";
	const char *sql_all = "SELECT COUNT(*) FROM model_history_items WHERE session_id=?";
	sqlite3_stmt *stmt;
	int count = 0;
	int rc;

	if (!db || !db->handle || session_id <= 0)
		return -EINVAL;
	rc = sqlite3_prepare_v2(db->handle, active_only ? sql_active : sql_all, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

int model_history_deactivate_turn(struct db *db, int64_t session_id, const char *turn_id)
{
	const char *sql = "UPDATE model_history_items SET active=0 "
			  "WHERE session_id=? AND turn_id=? AND active=1";
	sqlite3_stmt *stmt;
	int rc;

	if (!db || !db->handle || session_id <= 0 || !turn_id || !turn_id[0])
		return -EINVAL;
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, turn_id, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	return 0;
}

int model_history_deactivate_turn_except_user(struct db *db, int64_t session_id,
					      const char *turn_id)
{
	const char *sql = "UPDATE model_history_items SET active=0 "
			  "WHERE session_id=? AND turn_id=? AND active=1 "
			  "AND kind<>'user_message'";
	sqlite3_stmt *stmt;
	int rc;

	if (!db || !db->handle || session_id <= 0 || !turn_id || !turn_id[0])
		return -EINVAL;
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, turn_id, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	return 0;
}

int model_history_compaction_count(struct db *db, int64_t session_id)
{
	sqlite3_stmt *stmt;
	int count = 0;
	int rc;

	if (!db || !db->handle || session_id <= 0)
		return -EINVAL;
	rc = sqlite3_prepare_v2(db->handle,
				"SELECT COUNT(*) FROM history_compactions WHERE session_id=?", -1,
				&stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

int model_history_compaction_attempt_add(struct db *db, int64_t session_id, const char *turn_id,
					 const char *trigger_kind, const char *status,
					 int input_tokens, int output_tokens, int error_code,
					 const char *error_text)
{
	const char *sql = "INSERT INTO history_compaction_attempts(session_id,turn_id,"
			  "trigger_kind,status,input_tokens,output_tokens,error_code,"
			  "error_text,created_at) VALUES(?,?,?,?,?,?,?,?,strftime('%s','now'))";
	sqlite3_stmt *stmt;
	int rc;

	if (!db || !db->handle || session_id <= 0 || !trigger_kind || !status)
		return -EINVAL;
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
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
		return MORPH_ERR_DB;
	return 0;
}

int model_history_migrate_messages(struct db *db, int64_t session_id)
{
	const char *sql = "INSERT INTO model_history_items("
			  "session_id,sequence_no,turn_id,kind,role,content,idempotency_key,"
			  "token_count,"
			  "truncated,active,created_at) "
			  "SELECT session_id,ROW_NUMBER() OVER (ORDER BY created_at,id),"
			  "turn_id,CASE WHEN role='user' THEN 'user_message' "
			  "ELSE 'assistant_message' END,role,content,"
			  "'legacy:message:'||id,token_count,0,1,created_at "
			  "FROM messages WHERE session_id=?1 AND NOT EXISTS("
			  "SELECT 1 FROM model_history_items WHERE session_id=?1) "
			  "ORDER BY created_at,id";
	sqlite3_stmt *stmt;
	int count;
	int rc;

	if (!db || !db->handle || session_id <= 0)
		return -EINVAL;
	count = model_history_count(db, session_id, 0);
	if (count < 0)
		return count;
	if (count > 0)
		return 0;
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	return 0;
}

static int64_t *history_preserved_add(morph_array_t *preserved, int64_t id)
{
	for (size_t i = 0; i < preserved->nelts; i++) {
		int64_t *existing = morph_array_borrow(preserved, i);

		if (*existing == id)
			return existing;
	}
	int64_t *slot = morph_array_push_borrow(preserved);

	if (slot)
		*slot = id;
	return slot;
}

/* Selected rows share budgets even when several policies select the same row. */
static int history_preserved_contains(const morph_array_t *preserved, int64_t id)
{
	for (size_t i = 0; i < preserved->nelts; i++) {
		const int64_t *existing = morph_array_borrow(preserved, i);

		if (*existing == id)
			return 1;
	}
	return 0;
}

static int history_preserve_user_pairs(struct db *db, int64_t session_id, morph_array_t *preserved,
				       int *budget)
{
	const char *sql = "SELECT u.id,MAX(u.token_count,0),a.id,MAX(a.token_count,0) "
			  "FROM model_history_items u LEFT JOIN model_history_items a "
			  "ON a.id=(SELECT id FROM model_history_items "
			  "WHERE session_id=u.session_id AND turn_id=u.turn_id "
			  "AND kind='assistant_message' AND sequence_no>u.sequence_no "
			  "ORDER BY sequence_no DESC LIMIT 1) "
			  "WHERE u.session_id=? AND u.active=1 AND u.kind='user_message' "
			  "ORDER BY u.sequence_no DESC";
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	int error = 0;

	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		int64_t user_id = sqlite3_column_int64(stmt, 0);
		int64_t answer_id = sqlite3_column_int64(stmt, 2);
		int64_t cost = 0;

		if (!history_preserved_contains(preserved, user_id))
			cost += sqlite3_column_int(stmt, 1);
		if (answer_id && !history_preserved_contains(preserved, answer_id))
			cost += sqlite3_column_int(stmt, 3);
		if (cost < 0)
			cost = 0;
		if (cost > *budget)
			continue;
		if (!history_preserved_add(preserved, user_id) ||
		    (answer_id && !history_preserved_add(preserved, answer_id))) {
			error = (-ENOMEM);
			break;
		}
		*budget -= (int)cost;
	}
	sqlite3_finalize(stmt);
	if (error)
		return error;
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	return 0;
}

/* Full recent rounds are preferred, but never override the token budget. */
static int history_preserve_rounds(struct db *db, int64_t session_id, morph_array_t *preserved,
				   int rounds, int *budget)
{
	const char *sql = "SELECT turn_id FROM model_history_items WHERE session_id=? "
			  "AND active=1 AND turn_id IS NOT NULL AND turn_id<>'' "
			  "AND kind<>'compaction_summary' GROUP BY turn_id "
			  "ORDER BY MAX(sequence_no) DESC LIMIT ?";
	const char *rows_sql = "SELECT id,MAX(token_count,0) FROM model_history_items "
			       "WHERE session_id=? AND turn_id=? AND kind<>'compaction_summary' "
			       "AND (active=1 OR kind='assistant_message')";
	sqlite3_stmt *turns = NULL;
	sqlite3_stmt *rows = NULL;
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &turns, NULL);
	int error = 0;

	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	rc = sqlite3_prepare_v2(db->handle, rows_sql, -1, &rows, NULL);
	if (rc != SQLITE_OK) {
		sqlite3_finalize(turns);
		return MORPH_ERR_DB;
	}
	sqlite3_bind_int64(turns, 1, session_id);
	sqlite3_bind_int(turns, 2, rounds);
	while ((rc = sqlite3_step(turns)) == SQLITE_ROW) {
		const char *turn = (const char *)sqlite3_column_text(turns, 0);
		int64_t cost = 0;
		int row_rc;

		sqlite3_bind_int64(rows, 1, session_id);
		sqlite3_bind_text(rows, 2, turn, -1, SQLITE_TRANSIENT);
		while ((row_rc = sqlite3_step(rows)) == SQLITE_ROW) {
			if (!history_preserved_contains(preserved, sqlite3_column_int64(rows, 0)))
				cost += sqlite3_column_int(rows, 1);
		}
		if (row_rc != SQLITE_DONE) {
			error = (MORPH_ERR_DB);
			break;
		}
		sqlite3_reset(rows);
		if (cost > *budget) {
			rc = SQLITE_DONE;
			break;
		}
		while ((row_rc = sqlite3_step(rows)) == SQLITE_ROW) {
			if (!history_preserved_add(preserved, sqlite3_column_int64(rows, 0))) {
				error = (-ENOMEM);
				break;
			}
		}
		if (!error && row_rc != SQLITE_DONE)
			error = (MORPH_ERR_DB);
		if (error)
			break;
		sqlite3_reset(rows);
		sqlite3_clear_bindings(rows);
		*budget -= (int)cost;
	}
	sqlite3_finalize(rows);
	sqlite3_finalize(turns);
	if (error)
		return error;
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	return 0;
}

static int history_select_retained_apply(struct db *db, int64_t session_id, int user_message_tokens,
				  int recent_history_tokens, int keep_recent_rounds,
				  morph_array_t *preserved)
{
	sqlite3_stmt *stmt MORPH_CLEANUP(history_stmt_cleanup) = NULL;
	int recent_used_tokens = 0;
	int rc;

	/* User retention is a cap, not a reservation that wastes unused capacity.
	 * Select pairs first, then spend the remaining combined budget once. */
	rc = history_preserve_user_pairs(db, session_id, preserved, &user_message_tokens);
	if (rc == 0) {
		recent_history_tokens = user_message_tokens > INT_MAX - recent_history_tokens
						? INT_MAX
						: recent_history_tokens + user_message_tokens;
		rc = history_preserve_rounds(db, session_id, preserved, keep_recent_rounds,
					     &recent_history_tokens);
	}
	if (rc != 0) {
		return rc;
	}
	if (recent_history_tokens > 0) {
		const char *recent_sql = "SELECT id,kind,token_count FROM model_history_items "
					 "WHERE session_id=? AND active=1 "
					 "AND kind NOT IN ('user_message','compaction_summary') "
					 "ORDER BY sequence_no DESC";
		int need_tool_call = 0;
		size_t group_start = preserved->nelts;

		rc = sqlite3_prepare_v2(db->handle, recent_sql, -1, &stmt, NULL);
		if (rc != SQLITE_OK)
			return MORPH_ERR_DB;
		sqlite3_bind_int64(stmt, 1, session_id);
		while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
			const char *kind = (const char *)sqlite3_column_text(stmt, 1);
			int tokens = sqlite3_column_int(stmt, 2);
			int64_t *id;

			if (history_preserved_contains(preserved, sqlite3_column_int64(stmt, 0)))
				continue;
			if (tokens < 0)
				tokens = 0;
			if (tokens > recent_history_tokens - recent_used_tokens) {
				preserved->nelts = group_start;
				rc = SQLITE_DONE;
				break;
			}
			id = history_preserved_add(preserved, sqlite3_column_int64(stmt, 0));
			if (!id) {
				rc = SQLITE_NOMEM;
				break;
			}
			recent_used_tokens += tokens;
			if (kind && strcmp(kind, "tool_result") == 0)
				need_tool_call = 1;
			else if (need_tool_call && kind &&
				 strcmp(kind, "assistant_tool_calls") == 0)
				need_tool_call = 0;
			if (!need_tool_call)
				group_start = preserved->nelts;
			if (!need_tool_call && recent_used_tokens >= recent_history_tokens) {
				rc = SQLITE_DONE;
				break;
			}
		}
		if (need_tool_call)
			preserved->nelts = group_start;
		history_stmt_cleanup(&stmt);
		if (rc != SQLITE_DONE)
			return MORPH_ERR_DB;
	}
	return 0;
}

int model_history_select_retained(struct db *db, int64_t session_id, int user_message_tokens,
				  int recent_history_tokens, int keep_recent_rounds,
				  morph_array_t *preserved)
{
	if (!db || !db->handle || session_id <= 0 || !preserved || user_message_tokens < 0 ||
	    recent_history_tokens < 0 || keep_recent_rounds < 0)
		return -EINVAL;
	int rc = morph_array_init(preserved, 8, sizeof(int64_t));

	if (rc != 0)
		return rc;
	rc = history_select_retained_apply(db, session_id, user_message_tokens,
					   recent_history_tokens, keep_recent_rounds, preserved);
	if (rc != 0)
		morph_array_cleanup(preserved);
	return rc;
}

static int history_compact_apply(struct db *db, int64_t session_id, const char *turn_id,
				  const char *summary, int summary_tokens, int user_message_tokens,
				  int recent_history_tokens, int input_tokens,
				  int keep_recent_rounds, const char *trigger_kind,
				  int64_t *summary_item_id, const morph_array_t *selected,
				  int64_t expected_cutoff, morph_array_t *preserved)
{
	const char *inactive_sql = "UPDATE model_history_items SET active=0 "
				   "WHERE session_id=? AND active=1";
	const char *restore_sql = "UPDATE model_history_items SET active=1 WHERE id=?";
	const char *checkpoint_sql =
		"INSERT INTO history_compactions(session_id,turn_id,"
		"cutoff_sequence_no,summary_item_id,input_tokens,output_tokens,"
		"trigger_kind,created_at) VALUES(?,?,?,?,?,?,?,strftime('%s','now'))";
	struct model_history_insert insert = {0};
	char compact_key[256];
	sqlite3_stmt *stmt MORPH_CLEANUP(history_stmt_cleanup) = NULL;
	int64_t new_id = 0;
	int64_t cutoff_sequence_no = 0;
	int rc;

	if (selected) {
		int64_t *ids = morph_array_push_n_borrow(preserved, selected->nelts);
		if (selected->nelts && !ids) {
			return -ENOMEM;
		}
		if (selected->nelts)
			memcpy(ids, selected->elts, selected->nelts * sizeof(*ids));
	} else {
		morph_array_cleanup(preserved);
		rc = model_history_select_retained(db, session_id, user_message_tokens,
						   recent_history_tokens, keep_recent_rounds,
						   preserved);
		if (rc != 0) {
			return rc;
		}
	}
	rc = sqlite3_prepare_v2(db->handle,
				"SELECT COALESCE(MAX(sequence_no),0) FROM model_history_items "
				"WHERE session_id=?",
				-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		cutoff_sequence_no = sqlite3_column_int64(stmt, 0);
	else
		rc = SQLITE_ERROR;
	history_stmt_cleanup(&stmt);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	if (selected && cutoff_sequence_no != expected_cutoff) {
		return -EAGAIN;
	}
	rc = sqlite3_prepare_v2(db->handle, inactive_sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	history_stmt_cleanup(&stmt);
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	for (size_t i = 0; i < preserved->nelts; i++) {
		int64_t *id = morph_array_borrow(preserved, i);

		rc = sqlite3_prepare_v2(db->handle, restore_sql, -1, &stmt, NULL);
		if (rc != SQLITE_OK)
			return MORPH_ERR_DB;
		sqlite3_bind_int64(stmt, 1, *id);
		rc = sqlite3_step(stmt);
		history_stmt_cleanup(&stmt);
		if (rc != SQLITE_DONE)
			return MORPH_ERR_DB;
	}
	insert.session_id = session_id;
	insert.turn_id = turn_id;
	insert.kind = "compaction_summary";
	insert.role = "system";
	insert.content = summary;
	snprintf(compact_key, sizeof(compact_key), "compaction:%s:%s:%lld",
		 turn_id ? turn_id : "unknown", trigger_kind, (long long)cutoff_sequence_no);
	insert.idempotency_key = compact_key;
	insert.token_count = summary_tokens;
	insert.active = 1;
	rc = model_history_add(db, &insert, &new_id);
	if (rc != 0)
		return MORPH_ERR_DB;
	rc = sqlite3_prepare_v2(db->handle, checkpoint_sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
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
	history_stmt_cleanup(&stmt);
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	if (summary_item_id)
		*summary_item_id = new_id;
	return 0;
}

static int history_compact_window(struct db *db, int64_t session_id, const char *turn_id,
				  const char *summary, int summary_tokens, int user_message_tokens,
				  int recent_history_tokens, int input_tokens,
				  int keep_recent_rounds, const char *trigger_kind,
				  int64_t *summary_item_id, const morph_array_t *selected,
				  int64_t expected_cutoff)
{
	struct db_lock lock MORPH_CLEANUP(db_lock_cleanup) = {0};
	morph_array_t preserved MORPH_CLEANUP(morph_array_cleanup) = {0};
	int64_t new_id = 0;

	if (!db || !db->handle || session_id <= 0 || !summary || !summary[0] ||
	    summary_tokens < 0 || user_message_tokens < 0 || recent_history_tokens < 0 ||
	    keep_recent_rounds < 0 || !trigger_kind || !trigger_kind[0])
		return -EINVAL;
	int rc = morph_array_init(&preserved, 8, sizeof(int64_t));

	if (rc != 0)
		return rc;
	rc = db_lock_acquire(db, &lock);
	if (rc == 0)
		rc = db_exec(db, "SAVEPOINT model_history_compact;");
	if (rc != 0)
		return rc;
	rc = history_compact_apply(db, session_id, turn_id, summary, summary_tokens,
				   user_message_tokens, recent_history_tokens, input_tokens,
				   keep_recent_rounds, trigger_kind, &new_id, selected,
				   expected_cutoff, &preserved);
	if (rc == 0 && db_exec(db, "RELEASE model_history_compact;") != 0)
		rc = MORPH_ERR_DB;
	if (rc != 0) {
		(void)db_exec(db,
			"ROLLBACK TO model_history_compact; RELEASE model_history_compact;");
		return rc;
	}
	if (summary_item_id)
		*summary_item_id = new_id;
	return 0;
}

int model_history_compact(struct db *db, int64_t session_id, const char *turn_id,
			  const char *summary, int summary_tokens, int user_message_tokens,
			  int recent_history_tokens, int input_tokens, int keep_recent_rounds,
			  const char *trigger_kind, int64_t *summary_item_id)
{
	return history_compact_window(db, session_id, turn_id, summary, summary_tokens,
				      user_message_tokens, recent_history_tokens, input_tokens,
				      keep_recent_rounds, trigger_kind, summary_item_id, NULL, 0);
}

int model_history_compact_selected(struct db *db, int64_t session_id, const char *turn_id,
				   const char *summary, int summary_tokens, int input_tokens,
				   const char *trigger_kind, int64_t *summary_item_id,
				   const morph_array_t *preserved, int64_t cutoff)
{
	if (!preserved)
		return -EINVAL;
	return history_compact_window(db, session_id, turn_id, summary, summary_tokens, 0, 0,
				      input_tokens, 0, trigger_kind, summary_item_id, preserved,
				      cutoff);
}
