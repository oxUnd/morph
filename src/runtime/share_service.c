#include "runtime/runtime_internal.h"
#include "util/error.h"
#include <stdlib.h>
#include <string.h>

static const char *share_column(sqlite3_stmt *stmt, int column)
{
	const char *text = (const char *)sqlite3_column_text(stmt, column);
	return text ? text : "";
}

static const char *share_string(const cJSON *object, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
	return cJSON_IsString(value) ? value->valuestring : "";
}

static int share_prepare(sqlite3 *db, const char *sql, int64_t session_id,
			 sqlite3_stmt **out)
{
	if (sqlite3_prepare_v2(db, sql, -1, out, NULL) != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(*out, 1, session_id);
	return 0;
}

static cJSON *share_document(const char *answer)
{
	cJSON *document = cJSON_CreateObject();
	if (!document || !cJSON_AddArrayToObject(document, "questions") ||
	    !cJSON_AddArrayToObject(document, "steps") ||
	    !cJSON_AddStringToObject(document, "answer", answer)) {
		cJSON_Delete(document);
		return NULL;
	}
	return document;
}

static int share_question(cJSON *document, const char *text)
{
	cJSON *question = cJSON_CreateString(text);
	if (!question || !cJSON_AddItemToArray(
		cJSON_GetObjectItemCaseSensitive(document, "questions"), question)) {
		cJSON_Delete(question);
		MORPH_RETURN(-ENOMEM);
	}
	return 0;
}

static int share_step(cJSON *document, sqlite3_stmt *row)
{
	const char *metadata = share_column(row, 2);
	cJSON *payload = metadata[0] ? cJSON_Parse(metadata) : NULL;
	if (metadata[0] && !cJSON_IsObject(payload)) {
		cJSON_Delete(payload);
		MORPH_RETURN(MORPH_ERR_PARSE);
	}
	const char *status = share_string(payload, "status");
	cJSON *step = cJSON_CreateObject();
	int ok = step && cJSON_AddStringToObject(step, "name", share_column(row, 3)) &&
		cJSON_AddStringToObject(step, "status", status[0] ? status : "recorded") &&
		cJSON_AddStringToObject(step, "content", share_column(row, 1)) &&
		cJSON_AddBoolToObject(step, "truncated", sqlite3_column_int(row, 4));
	cJSON_Delete(payload);
	if (!ok || !cJSON_AddItemToArray(
		cJSON_GetObjectItemCaseSensitive(document, "steps"), step)) {
		cJSON_Delete(step);
		MORPH_RETURN(-ENOMEM);
	}
	return 0;
}

/* Read only the questions for this candidate, never unrelated tool output. */
static int share_turn_complete(sqlite3 *db, int64_t session_id,
			       const char *turn_id, int64_t sequence)
{
	sqlite3_stmt *stmt = NULL;
	int rc = share_prepare(db,
		"SELECT payload_json FROM model_history_items WHERE session_id=? "
		"AND turn_id=? AND sequence_no<? AND kind='user_message'",
		session_id, &stmt);
	if (rc)
		MORPH_RETURN(rc);
	sqlite3_bind_text(stmt, 2, turn_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, sequence);
	int found = 0;
	int result;
	while ((result = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *metadata = share_column(stmt, 0);
		cJSON *payload = metadata[0] ? cJSON_Parse(metadata) : NULL;
		if (metadata[0] && !cJSON_IsObject(payload)) {
			cJSON_Delete(payload);
			MORPH_SET_ERR(rc, MORPH_ERR_PARSE);
			break;
		}
		const cJSON *outcome = cJSON_GetObjectItemCaseSensitive(payload, "turn_outcome");
		const char *status = share_string(outcome, "status");
		int failed = status[0] && strcmp(status, "success") != 0 &&
			strcmp(status, "answered") != 0;
		cJSON_Delete(payload);
		if (failed) {
			found = 0;
			break;
		}
		found = 1;
	}
	if (!rc && result != SQLITE_ROW && result != SQLITE_DONE)
		MORPH_SET_ERR(rc, MORPH_ERR_DB);
	sqlite3_finalize(stmt);
	MORPH_RETURN(rc ? rc : found);
}

static int share_turn_read(sqlite3 *db, int64_t session_id, const char *turn_id,
			   int64_t sequence, cJSON *document)
{
	sqlite3_stmt *stmt = NULL;
	int rc = share_prepare(db,
		"SELECT kind,content,payload_json,tool_name,truncated "
		"FROM model_history_items WHERE session_id=? AND turn_id=? "
		"AND sequence_no<? AND kind IN ('user_message','tool_result') "
		"ORDER BY sequence_no", session_id, &stmt);
	if (rc)
		MORPH_RETURN(rc);
	sqlite3_bind_text(stmt, 2, turn_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, sequence);
	int result;
	while ((result = sqlite3_step(stmt)) == SQLITE_ROW) {
		rc = strcmp(share_column(stmt, 0), "user_message") == 0 ?
			share_question(document, share_column(stmt, 1)) : share_step(document, stmt);
		if (rc)
			break;
	}
	if (!rc && result != SQLITE_DONE)
		MORPH_SET_ERR(rc, MORPH_ERR_DB);
	sqlite3_finalize(stmt);
	MORPH_RETURN(rc);
}

/* Migrated messages have no turn ID; their nearest preceding user is the boundary. */
static int share_migrated_question(sqlite3 *db, int64_t session_id,
				   int64_t sequence, cJSON *document)
{
	sqlite3_stmt *stmt = NULL;
	int rc = share_prepare(db,
		"SELECT content FROM model_history_items WHERE session_id=? "
		"AND sequence_no<? AND kind='user_message' "
		"ORDER BY sequence_no DESC LIMIT 1", session_id, &stmt);
	if (rc)
		MORPH_RETURN(rc);
	sqlite3_bind_int64(stmt, 2, sequence);
	int result = sqlite3_step(stmt);
	if (result == SQLITE_ROW)
		rc = share_question(document, share_column(stmt, 0));
	else
		MORPH_SET_ERR(rc, result == SQLITE_DONE ? -ENODATA : MORPH_ERR_DB);
	sqlite3_finalize(stmt);
	MORPH_RETURN(rc);
}

static int share_history(sqlite3 *db, int64_t session_id, cJSON **out)
{
	sqlite3_stmt *stmt = NULL;
	int rc = share_prepare(db,
		"SELECT sequence_no,turn_id,content,idempotency_key FROM model_history_items "
		"WHERE session_id=? AND kind='assistant_message' AND length(content)>0 "
		"ORDER BY sequence_no DESC", session_id, &stmt);
	if (rc)
		MORPH_RETURN(rc);
	static const char legacy_prefix[] = "legacy:message:";
	int result;
	while ((result = sqlite3_step(stmt)) == SQLITE_ROW) {
		int64_t sequence = sqlite3_column_int64(stmt, 0);
		const char *turn = share_column(stmt, 1);
		int migrated = !turn[0] &&
			strncmp(share_column(stmt, 3), legacy_prefix, sizeof(legacy_prefix) - 1) == 0;
		int complete = turn[0] ? share_turn_complete(db, session_id, turn, sequence) : migrated;
		if (complete < 0) {
			MORPH_SET_ERR(rc, complete);
			break;
		}
		if (!complete)
			continue;
		cJSON *document = share_document(share_column(stmt, 2));
		if (!document) {
			MORPH_SET_ERR(rc, -ENOMEM);
			break;
		}
		rc = migrated ? share_migrated_question(db, session_id, sequence, document) :
			share_turn_read(db, session_id, turn, sequence, document);
		if (!rc) {
			*out = document;
			break;
		}
		cJSON_Delete(document);
		if (rc != -ENODATA)
			break;
		rc = 0;
	}
	if (!rc && !*out)
		MORPH_SET_ERR(rc, result == SQLITE_DONE ? -ENODATA : MORPH_ERR_DB);
	sqlite3_finalize(stmt);
	MORPH_RETURN(rc);
}

static int share_legacy(sqlite3 *db, int64_t session_id, cJSON **out)
{
	sqlite3_stmt *stmt = NULL;
	int rc = share_prepare(db,
		"SELECT a.content,u.content FROM messages a JOIN messages u ON u.id=("
		"SELECT id FROM messages WHERE session_id=a.session_id AND role='user' "
		"AND (created_at<a.created_at OR (created_at=a.created_at AND id<a.id)) "
		"ORDER BY created_at DESC,id DESC LIMIT 1) "
		"WHERE a.session_id=? AND a.role='assistant' AND length(a.content)>0 "
		"AND NOT EXISTS(SELECT 1 FROM model_history_items WHERE session_id=a.session_id) "
		"ORDER BY a.created_at DESC,a.id DESC LIMIT 1", session_id, &stmt);
	if (rc)
		MORPH_RETURN(rc);
	int result = sqlite3_step(stmt);
	if (result == SQLITE_ROW) {
		cJSON *document = share_document(share_column(stmt, 0));
		rc = document ? share_question(document, share_column(stmt, 1)) : -ENOMEM;
		if (!rc)
			*out = document;
		else
			cJSON_Delete(document);
	} else {
		MORPH_SET_ERR(rc, result == SQLITE_DONE ? -ENODATA : MORPH_ERR_DB);
	}
	sqlite3_finalize(stmt);
	MORPH_RETURN(rc);
}

int runtime_session_share_current(struct runtime *runtime, cJSON **out)
{
	if (!out)
		MORPH_RETURN(-EINVAL);
	*out = NULL;
	if (!runtime)
		MORPH_RETURN(-EINVAL);
	int rc = pthread_mutex_trylock(&runtime->context.execution_lock);
	if (rc)
		MORPH_RETURN(-rc);
	struct runtime_context *ctx = &runtime->context;
	rc = share_history(ctx->database.handle, ctx->current_session.id, out);
	if (rc == -ENODATA)
		rc = share_legacy(ctx->database.handle, ctx->current_session.id, out);
	pthread_mutex_unlock(&ctx->execution_lock);
	MORPH_RETURN(rc);
}
