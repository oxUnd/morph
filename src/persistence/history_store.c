#include "history_store.h"
#include "util/cleanup.h"
#include "util/error.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int prepare_session_query(struct db *db, const char *sql, int64_t session_id,
				 sqlite3_stmt **stmt)
{
	if (!db || !db->handle || session_id <= 0 || !stmt)
		return -EINVAL;
	if (sqlite3_prepare_v2(db->handle, sql, -1, stmt, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	if (sqlite3_bind_int64(*stmt, 1, session_id) != SQLITE_OK) {
		sqlite3_finalize(*stmt);
		*stmt = NULL;
		return MORPH_ERR_DB;
	}
	return 0;
}

static const char *column_text(sqlite3_stmt *stmt, int column)
{
	return (const char *)sqlite3_column_text(stmt, column);
}

static int finish_query(sqlite3_stmt *stmt, int step, int error)
{
	sqlite3_finalize(stmt);
	if (error)
		return error < 0 ? error : 0;
	return step == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

int history_store_context_tokens(struct db *db, int64_t session_id, int64_t *tokens)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!tokens)
		return -EINVAL;
	*tokens = 0;
	rc = prepare_session_query(
		db,
		"SELECT COALESCE(SUM(CASE WHEN active=1 THEN token_count ELSE 0 END),0),"
		"COUNT(*),EXISTS(SELECT 1 FROM messages WHERE session_id=?1) "
		"FROM model_history_items WHERE session_id=?1",
		session_id, &stmt);
	if (rc != 0)
		return rc;
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		*tokens = sqlite3_column_int64(stmt, 0);
		rc = sqlite3_column_int64(stmt, 1) == 0 && sqlite3_column_int(stmt, 2) ? -ENODATA
										       : 0;
	} else {
		rc = MORPH_ERR_DB;
	}
	sqlite3_finalize(stmt);
	return rc;
}

static int read_text(struct db *db, int64_t session_id, const char *sql, int64_t sequence,
		     char **out)
{
	char *text_value MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	sqlite3_stmt *stmt = NULL;
	int rc = prepare_session_query(db, sql, session_id, &stmt);

	if (rc != 0)
		return rc;
	if (sqlite3_bind_parameter_count(stmt) > 1)
		sqlite3_bind_int64(stmt, 2, sequence);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		const char *text = column_text(stmt, 0);

		text_value = strdup(text ? text : "");
		rc = text_value ? 0 : -ENOMEM;
	} else {
		rc = rc == SQLITE_DONE ? -ENODATA : MORPH_ERR_DB;
	}
	sqlite3_finalize(stmt);
	if (rc == 0)
		*out = morph_charp_steal(&text_value);
	return rc;
}

int history_store_last_answer_copy(struct db *db, int64_t session_id, char **answer)
{
	int rc;

	if (!answer)
		return -EINVAL;
	if (*answer)
		return -EEXIST;
	rc = read_text(db, session_id,
		       "SELECT content FROM model_history_items WHERE session_id=? "
		       "AND kind='assistant_message' AND length(content)>0 "
		       "ORDER BY sequence_no DESC LIMIT 1",
		       0, answer);
	if (rc == -ENODATA)
		rc = read_text(
			db, session_id,
			"SELECT content FROM messages WHERE session_id=? AND role='assistant' "
			"AND length(content)>0 ORDER BY created_at DESC,id DESC LIMIT 1",
			0, answer);
	return rc;
}

int history_store_previous_question_copy(struct db *db, int64_t session_id, int64_t before_sequence,
					 char **question)
{
	if (!question)
		return -EINVAL;
	if (*question)
		return -EEXIST;
	return read_text(db, session_id,
			 "SELECT content FROM model_history_items WHERE session_id=? "
			 "AND sequence_no<? AND kind='user_message' "
			 "ORDER BY sequence_no DESC LIMIT 1",
			 before_sequence, question);
}

int history_store_summary_sources(struct db *db, int64_t session_id, morph_array_t *ids)
{
	sqlite3_stmt *stmt = NULL;
	int step;
	int rc;

	if (!ids)
		return -EINVAL;
	rc = prepare_session_query(
		db,
		"WITH RECURSIVE covered(id) AS ("
		"SELECT s.source_item_id FROM history_summary_sources s "
		"JOIN model_history_items m ON m.id=s.summary_item_id "
		"WHERE m.session_id=? AND m.active=1 AND m.kind='compaction_summary' "
		"UNION SELECT s.source_item_id FROM history_summary_sources s "
		"JOIN covered c ON s.summary_item_id=c.id) SELECT id FROM covered",
		session_id, &stmt);
	if (rc != 0)
		return rc;
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		int64_t *id = morph_array_push_borrow(ids);

		if (!id) {
			rc = -ENOMEM;
			break;
		}
		*id = sqlite3_column_int64(stmt, 0);
	}
	return finish_query(stmt, step, rc);
}

void history_request_snapshot_cleanup(struct history_request_snapshot *snapshot)
{
	if (!snapshot)
		return;
	free(snapshot->turn_id);
	free(snapshot->provider);
	free(snapshot->model);
	free(snapshot->input_json);
	memset(snapshot, 0, sizeof(*snapshot));
}

static void history_request_snapshot_move(struct history_request_snapshot *destination,
					  struct history_request_snapshot *source)
{
	*destination = *source;
	memset(source, 0, sizeof(*source));
}

int history_store_request_snapshot(struct db *db, int64_t session_id,
				   struct history_request_snapshot *out)
{
	struct history_request_snapshot value MORPH_CLEANUP(history_request_snapshot_cleanup) = {0};
	struct history_request_snapshot *snapshot = &value;
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!out)
		return -EINVAL;
	if (out->turn_id || out->provider || out->model || out->input_json)
		return -EEXIST;
	rc = prepare_session_query(
		db,
		"SELECT turn_id,provider,model,input_json,created_at FROM model_request_snapshots "
		"WHERE session_id=?",
		session_id, &stmt);
	if (rc != 0)
		return rc;
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		char **fields[] = {
			&snapshot->turn_id,
			&snapshot->provider,
			&snapshot->model,
			&snapshot->input_json,
		};

		for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
			const char *text = column_text(stmt, (int)i);

			*fields[i] = strdup(text ? text : "");
			if (!*fields[i]) {
				rc = SQLITE_NOMEM;
				break;
			}
		}
		snapshot->created_at = sqlite3_column_int64(stmt, 4);
	}
	sqlite3_finalize(stmt);
	if (rc == SQLITE_ROW)
		history_request_snapshot_move(out, &value);
	return rc == SQLITE_ROW	    ? 0
	       : rc == SQLITE_DONE  ? -ENODATA
	       : rc == SQLITE_NOMEM ? -ENOMEM
				    : MORPH_ERR_DB;
}

static int visit_messages(struct db *db, int64_t session_id, const char *sql, int limit,
			  history_message_visitor visitor, void *data)
{
	sqlite3_stmt *stmt = NULL;
	int step;
	int rc;

	if (!visitor)
		return -EINVAL;
	rc = prepare_session_query(db, sql, session_id, &stmt);
	if (rc != 0)
		return rc;
	if (limit > 0)
		sqlite3_bind_int(stmt, 2, limit);
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		struct message message = {0};
		const char *role = column_text(stmt, 1);

		message.id = sqlite3_column_int64(stmt, 0);
		message.session_id = session_id;
		snprintf(message.role, sizeof(message.role), "%s", role ? role : "");
		message.content = (char *)column_text(stmt, 2);
		message.turn_id = (char *)column_text(stmt, 3);
		message.created_at = sqlite3_column_int64(stmt, 4);
		rc = visitor(&message, data);
		if (rc != 0)
			break;
	}
	return finish_query(stmt, step, rc);
}

int history_store_visit_messages(struct db *db, int64_t session_id, history_message_visitor visitor,
				 void *data)
{
	return visit_messages(db, session_id,
			      "SELECT id,role,content,turn_id,created_at FROM messages "
			      "WHERE session_id=? ORDER BY created_at,id",
			      0, visitor, data);
}

int history_store_visit_recent_messages(struct db *db, int64_t session_id, int limit,
					history_message_visitor visitor, void *data)
{
	if (limit <= 0)
		return -EINVAL;
	return visit_messages(
		db, session_id,
		"SELECT id,role,content,turn_id,created_at FROM messages WHERE session_id=? "
		"AND role IN ('user','assistant') AND length(trim(content))>0 "
		"ORDER BY id DESC LIMIT ?",
		limit, visitor, data);
}

int history_store_visit_display(struct db *db, int64_t session_id, history_display_visitor visitor,
				void *data)
{
	sqlite3_stmt *stmt = NULL;
	int step;
	int rc;

	if (!visitor)
		return -EINVAL;
	rc = prepare_session_query(
		db,
		"SELECT 1 FROM sqlite_master WHERE type='table' AND name='sync_ui_messages' "
		"AND ?1>0",
		session_id, &stmt);
	if (rc != 0)
		return rc;
	step = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	stmt = NULL;
	if (step != SQLITE_ROW)
		return step == SQLITE_DONE ? -ENODATA : MORPH_ERR_DB;
	rc = prepare_session_query(
		db,
		"SELECT type,content,attachments_json,structured_data,agent_ui_ir,"
		"hitl_verdict,core_message_id,created_at,turn_id FROM sync_ui_messages "
		"WHERE session_id=? ORDER BY seq",
		session_id, &stmt);
	if (rc != 0)
		return rc;
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		struct history_display_record record = {
			.type = column_text(stmt, 0),
			.content = column_text(stmt, 1),
			.attachments_json = column_text(stmt, 2),
			.structured_data = column_text(stmt, 3),
			.agent_ui_ir = column_text(stmt, 4),
			.hitl_verdict = sqlite3_column_int64(stmt, 5),
			.core_message_id = sqlite3_column_int64(stmt, 6),
			.created_at = sqlite3_column_int64(stmt, 7),
			.turn_id = column_text(stmt, 8),
		};

		if (sqlite3_column_type(stmt, 5) != SQLITE_NULL)
			record.present |= HISTORY_DISPLAY_VERDICT;
		if (sqlite3_column_type(stmt, 6) != SQLITE_NULL)
			record.present |= HISTORY_DISPLAY_MESSAGE;
		if (sqlite3_column_type(stmt, 7) != SQLITE_NULL)
			record.present |= HISTORY_DISPLAY_CREATED;
		rc = visitor(&record, data);
		if (rc != 0)
			break;
	}
	return finish_query(stmt, step, rc);
}

int history_store_visit_traces(struct db *db, int64_t session_id, history_trace_visitor visitor,
			       void *data)
{
	sqlite3_stmt *stmt = NULL;
	int step;
	int rc;

	if (!visitor)
		return -EINVAL;
	rc = prepare_session_query(db,
				   "SELECT id,steps_json,created_at FROM react_traces "
				   "WHERE session_id=? AND aborted=0 ORDER BY created_at,id",
				   session_id, &stmt);
	if (rc != 0)
		return rc;
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		struct history_trace_record record = {
			.id = sqlite3_column_int64(stmt, 0),
			.steps_json = column_text(stmt, 1),
			.created_at = sqlite3_column_int64(stmt, 2),
		};

		rc = visitor(&record, data);
		if (rc != 0)
			break;
	}
	return finish_query(stmt, step, rc);
}

#define SHARE_COLUMNS                                                                              \
	"sequence_no,turn_id,kind,content,payload_json,tool_name,"                                 \
	"truncated,idempotency_key"

static int visit_share_items(struct db *db, int64_t session_id, const char *sql,
			     const char *turn_id, int64_t sequence, history_item_visitor visitor,
			     void *data)
{
	sqlite3_stmt *stmt = NULL;
	int step;
	int rc;

	if (!visitor)
		return -EINVAL;
	rc = prepare_session_query(db, sql, session_id, &stmt);
	if (rc != 0)
		return rc;
	if (turn_id) {
		sqlite3_bind_text(stmt, 2, turn_id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 3, sequence);
	}
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		struct model_history_item item = {0};
		const char *kind = column_text(stmt, 2);
		const char *tool = column_text(stmt, 5);

		item.sequence_no = sqlite3_column_int64(stmt, 0);
		item.turn_id = (char *)column_text(stmt, 1);
		snprintf(item.kind, sizeof(item.kind), "%s", kind ? kind : "");
		item.content = (char *)column_text(stmt, 3);
		item.payload_json = (char *)column_text(stmt, 4);
		snprintf(item.tool_name, sizeof(item.tool_name), "%s", tool ? tool : "");
		item.truncated = sqlite3_column_int(stmt, 6);
		item.idempotency_key = (char *)column_text(stmt, 7);
		rc = visitor(&item, data);
		if (rc != 0)
			break;
	}
	return finish_query(stmt, step, rc);
}

int history_store_visit_answers(struct db *db, int64_t session_id, history_item_visitor visitor,
				void *data)
{
	return visit_share_items(
		db, session_id,
		"SELECT " SHARE_COLUMNS " FROM model_history_items WHERE session_id=? "
		"AND kind='assistant_message' AND length(content)>0 ORDER BY sequence_no DESC",
		NULL, 0, visitor, data);
}

int history_store_visit_turn(struct db *db, int64_t session_id, const char *turn_id,
			     int64_t before_sequence, history_item_visitor visitor, void *data)
{
	if (!turn_id)
		return -EINVAL;
	return visit_share_items(
		db, session_id,
		"SELECT " SHARE_COLUMNS " FROM model_history_items WHERE session_id=? "
		"AND turn_id=? AND sequence_no<? AND kind IN ('user_message','tool_result') "
		"ORDER BY sequence_no",
		turn_id, before_sequence, visitor, data);
}

int history_store_legacy_answer_copy(struct db *db, int64_t session_id, char **answer,
				     char **question)
{
	char *answer_value MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	char *question_value MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!answer || !question || answer == question)
		return -EINVAL;
	if (*answer || *question)
		return -EEXIST;
	rc = prepare_session_query(
		db,
		"SELECT a.content,u.content FROM messages a JOIN messages u ON u.id=("
		"SELECT id FROM messages WHERE session_id=a.session_id AND role='user' "
		"AND (created_at<a.created_at OR (created_at=a.created_at AND id<a.id)) "
		"ORDER BY created_at DESC,id DESC LIMIT 1) "
		"WHERE a.session_id=? AND a.role='assistant' AND length(a.content)>0 "
		"AND NOT EXISTS(SELECT 1 FROM model_history_items WHERE session_id=a.session_id) "
		"ORDER BY a.created_at DESC,a.id DESC LIMIT 1",
		session_id, &stmt);
	if (rc != 0)
		return rc;
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		const char *answer_text = column_text(stmt, 0);
		const char *question_text = column_text(stmt, 1);

		answer_value = strdup(answer_text ? answer_text : "");
		question_value = strdup(question_text ? question_text : "");
		rc = answer_value && question_value ? 0 : -ENOMEM;
	} else {
		rc = rc == SQLITE_DONE ? -ENODATA : MORPH_ERR_DB;
	}
	sqlite3_finalize(stmt);
	if (rc == 0) {
		*answer = morph_charp_steal(&answer_value);
		*question = morph_charp_steal(&question_value);
	}
	return rc;
}

int history_store_visible_progress(struct db *db, int64_t session_id,
				   struct history_message_progress *out)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	rc = prepare_session_query(
		db,
		"SELECT COALESCE(MAX(id),0),COALESCE(SUM(role='assistant'),0) "
		"FROM messages WHERE session_id=? AND role IN ('user','assistant') "
		"AND length(trim(content))>0",
		session_id, &stmt);
	if (rc)
		return rc;
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		out->latest_id = sqlite3_column_int64(stmt, 0);
		out->completed_turns = sqlite3_column_int(stmt, 1);
	}
	sqlite3_finalize(stmt);
	return rc == SQLITE_ROW ? 0 : MORPH_ERR_DB;
}

int history_store_update_item(struct db *db, int64_t id, int active, int token_count)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!db || !db->handle || id <= 0 || token_count < 0)
		return -EINVAL;
	rc = sqlite3_prepare_v2(db->handle,
				"UPDATE model_history_items SET active=?,token_count=? WHERE id=?",
				-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int(stmt, 1, active);
	sqlite3_bind_int(stmt, 2, token_count);
	sqlite3_bind_int64(stmt, 3, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

int history_store_refresh_retention_tokens(struct db *db, int64_t session_id,
					   history_token_counter count, void *data)
{
	sqlite3_stmt *select = NULL;
	sqlite3_stmt *update = NULL;
	int rc;
	int error = 0;

	if (!count)
		return -EINVAL;
	rc = prepare_session_query(
		db,
		"SELECT a.id,a.kind,a.content,a.payload_json FROM model_history_items a "
		"WHERE a.session_id=? AND (a.active=1 OR (a.kind='assistant_message' "
		"AND EXISTS (SELECT 1 FROM model_history_items u "
		"WHERE u.session_id=a.session_id AND u.turn_id=a.turn_id "
		"AND u.active=1 AND u.kind='user_message')))",
		session_id, &select);
	if (rc)
		return rc;
	if (sqlite3_prepare_v2(db->handle,
			       "UPDATE model_history_items SET token_count=? WHERE id=?", -1,
			       &update, NULL) != SQLITE_OK) {
		sqlite3_finalize(select);
		return MORPH_ERR_DB;
	}
	while ((rc = sqlite3_step(select)) == SQLITE_ROW) {
		struct model_history_item item = {0};
		const char *kind = column_text(select, 1);

		item.id = sqlite3_column_int64(select, 0);
		item.session_id = session_id;
		snprintf(item.kind, sizeof(item.kind), "%s", kind ? kind : "");
		item.content = (char *)column_text(select, 2);
		item.payload_json = (char *)column_text(select, 3);
		int tokens = count(&item, data);

		if (tokens < 0) {
			error = tokens;
			break;
		}
		sqlite3_bind_int(update, 1, tokens);
		sqlite3_bind_int64(update, 2, item.id);
		if (sqlite3_step(update) != SQLITE_DONE) {
			error = MORPH_ERR_DB;
			break;
		}
		sqlite3_reset(update);
		sqlite3_clear_bindings(update);
	}
	sqlite3_finalize(update);
	return finish_query(select, rc, error);
}

int history_store_add_summary_sources(struct db *db, int64_t summary_id, const int64_t *ids,
				      size_t count)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!db || !db->handle || summary_id <= 0 || (count && !ids))
		return -EINVAL;
	rc = sqlite3_prepare_v2(
		db->handle,
		"INSERT INTO history_summary_sources(summary_item_id,source_item_id) VALUES(?,?)",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	for (size_t i = 0; i < count; i++) {
		sqlite3_bind_int64(stmt, 1, summary_id);
		sqlite3_bind_int64(stmt, 2, ids[i]);
		rc = sqlite3_step(stmt);
		if (rc != SQLITE_DONE)
			break;
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
	}
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE || rc == SQLITE_OK ? 0 : MORPH_ERR_DB;
}

int history_store_last_sequence(struct db *db, int64_t session_id, int64_t *sequence)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!sequence)
		return -EINVAL;
	*sequence = 0;
	rc = prepare_session_query(
		db,
		"SELECT COALESCE(MAX(sequence_no),0) FROM model_history_items WHERE session_id=?",
		session_id, &stmt);
	if (rc)
		return rc;
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW)
		*sequence = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	return rc == SQLITE_ROW ? 0 : MORPH_ERR_DB;
}
