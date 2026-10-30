#include "runtime/runtime_internal.h"

#include "runtime/session.h"
#include "util/error.h"
#include "cJSON.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int runtime_session_current(const struct runtime *runtime, struct session *out)
{
	if (!runtime || !out)
		return -EINVAL;
	*out = runtime->context.current_session;
	if (out->id > 0)
		return session_get_by_id(&runtime->context.database,
			out->id, out);
	if (out->id <= 0) {
		strncpy(out->model, runtime->context.config.models.text.model,
			sizeof(out->model) - 1);
		out->model[sizeof(out->model) - 1] = '\0';
	}
	return 0;
}

int runtime_session_current_id(const struct runtime *runtime, int64_t *out)
{
	if (!runtime || !out)
		return -EINVAL;
	*out = runtime->context.current_session.id;
	return 0;
}

int runtime_session_select(struct runtime *runtime, const char *name,
			   struct session *out, int *created)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	struct session session;
	int rc;

	if (!ctx || !name || !name[0])
		return -EINVAL;
	pthread_mutex_lock(&ctx->execution_lock);
	rc = runtime_session_switch(&ctx->engine, name,
			ctx->config.models.text.model, &session, created);
	if (rc == 0) {
		runtime_recap_invalidate(runtime);
		ctx->current_session = session;
		runtime_context_select_plan_session(ctx, session.id);
		(void)runtime_context_update_tool_runtime_context(ctx, session.id);
		/*
		 * Selecting a session is a navigation operation.  Loading and
		 * repairing its entire model history here makes large sessions slow
		 * to open, and agent_turn_begin() loads the same history again before
		 * the next model turn.  Drop the previous session's in-memory context
		 * now and defer the incoming history load until it is actually needed.
		 */
		runtime_session_clear_history(ctx->react);
		if (out)
			*out = session;
	}
	pthread_mutex_unlock(&ctx->execution_lock);
	return rc;
}

int runtime_session_create_and_select(struct runtime *runtime, const char *name,
				      struct session *out)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	struct session session;
	int rc;

	if (!ctx || !name || !name[0])
		return -EINVAL;
	pthread_mutex_lock(&ctx->execution_lock);
	rc = runtime_session_create(&ctx->engine, name,
			ctx->config.models.text.model, &session);
	if (rc == 0) {
		runtime_recap_invalidate(runtime);
		ctx->current_session = session;
		runtime_context_select_plan_session(ctx, session.id);
		(void)runtime_context_update_tool_runtime_context(ctx, session.id);
		runtime_session_clear_history(ctx->react);
		if (out)
			*out = session;
	}
	pthread_mutex_unlock(&ctx->execution_lock);
	return rc;
}

int runtime_session_create_detached(struct runtime *runtime, const char *name,
				    struct session *out)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	int rc;

	if (!ctx || !name || !name[0] || !out)
		return -EINVAL;
	pthread_mutex_lock(&ctx->execution_lock);
	rc = session_create(&ctx->database, name, ctx->config.models.text.model, out);
	if (rc == 0)
		(void)session_ensure_display_id(&ctx->database, out);
	pthread_mutex_unlock(&ctx->execution_lock);
	return rc;
}

int runtime_session_delete_and_update(struct runtime *runtime, int64_t id)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	int rc;

	if (!ctx || id <= 0)
		return -EINVAL;
	pthread_mutex_lock(&ctx->execution_lock);
	runtime_title_cancel(runtime, id);
	rc = runtime_session_delete(&ctx->engine, id);
	if (rc == 0) {
		runtime_recap_invalidate(runtime);
		runtime_context_forget_plan_session(ctx, id);
		if (ctx->current_session.id == id) {
			runtime_session_clear_history(ctx->react);
			memset(&ctx->current_session, 0, sizeof(ctx->current_session));
		}
	}
	pthread_mutex_unlock(&ctx->execution_lock);
	return rc;
}

int runtime_session_rename_and_update(struct runtime *runtime, int64_t id,
				      const char *name)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	int rc;

	if (!ctx || id <= 0 || !name || !name[0])
		return -EINVAL;
	pthread_mutex_lock(&ctx->execution_lock);
	runtime_title_cancel(runtime, id);
	rc = runtime_session_rename(&ctx->engine, id, name);
	if (rc == 0 && ctx->current_session.id == id) {
		strncpy(ctx->current_session.name, name,
			sizeof(ctx->current_session.name) - 1);
	}
	pthread_mutex_unlock(&ctx->execution_lock);
	return rc;
}

int runtime_session_list_all(struct runtime *runtime, struct session **out,
			     int *count, int recent_first)
{
	if (!runtime || !out || !count)
		return -EINVAL;
	return session_list(&runtime->context.database, out, count,
			    recent_first, NULL);
}

int runtime_session_context_tokens(struct runtime *runtime, int64_t session_id,
				   int64_t *out)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!runtime || session_id <= 0 || !out)
		return -EINVAL;
	*out = 0;
	rc = sqlite3_prepare_v2(runtime->context.database.handle,
		"SELECT COALESCE(SUM(CASE WHEN active=1 THEN token_count "
		"ELSE 0 END),0),COUNT(*),"
		"EXISTS(SELECT 1 FROM messages WHERE session_id=?1) "
		"FROM model_history_items WHERE session_id=?1", -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		*out = sqlite3_column_int64(stmt, 0);
		rc = sqlite3_column_int64(stmt, 1) == 0 &&
			sqlite3_column_int(stmt, 2) ? -ENODATA : 0;
	} else {
		rc = MORPH_ERR_DB;
	}
	sqlite3_finalize(stmt);
	return rc;
}

void runtime_session_list_free(struct session *sessions)
{
	free(sessions);
}

int runtime_session_reload_current(struct runtime *runtime)
{
	if (!runtime || runtime->context.current_session.id <= 0)
		return -EINVAL;
	runtime_session_load_history(&runtime->context.engine,
				     runtime->context.current_session.id);
	return 0;
}

const char *runtime_session_current_name(struct runtime *runtime)
{
	struct session session;

	if (!runtime)
		return NULL;
	pthread_mutex_lock(&runtime->context.execution_lock);
	if (runtime_session_current(runtime, &session) == 0)
		runtime->context.current_session = session;
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return runtime->context.current_session.name;
}

struct message *runtime_session_messages_current(struct runtime *runtime,
					 int *count)
{
	if (!runtime || !count || runtime->context.current_session.id <= 0)
		return NULL;
	return message_list(&runtime->context.database,
			    runtime->context.current_session.id, count);
}

void runtime_session_messages_free(struct message *messages)
{
	message_free_list(messages);
}

static int transcript_add_display(sqlite3 *db, int64_t session_id, cJSON *root)
{
	sqlite3_stmt *stmt = NULL;
	cJSON *items;
	int rc;
	int step;

	if (sqlite3_prepare_v2(db,
		"SELECT 1 FROM sqlite_master WHERE type='table' "
		"AND name='sync_ui_messages'", -1, &stmt, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	step = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (step == SQLITE_DONE)
		return 0;
	if (step != SQLITE_ROW)
		return MORPH_ERR_DB;
	items = cJSON_AddArrayToObject(root, "display");
	if (!items)
		return -ENOMEM;
	if (sqlite3_prepare_v2(db,
		"SELECT type,content,attachments_json,structured_data,agent_ui_ir,"
		"hitl_verdict,core_message_id,created_at,turn_id FROM sync_ui_messages "
		"WHERE session_id=? ORDER BY seq", -1, &stmt, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = 0;
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		cJSON *item = cJSON_CreateObject();
		if (!item) {
			rc = (-ENOMEM);
			break;
		}
		for (int i = 0; i < sqlite3_column_count(stmt); i++) {
			const char *key = sqlite3_column_name(stmt, i);
			cJSON *value;
			if (sqlite3_column_type(stmt, i) == SQLITE_NULL)
				continue;
			if (sqlite3_column_type(stmt, i) == SQLITE_INTEGER)
				value = cJSON_AddNumberToObject(item, key,
					(double)sqlite3_column_int64(stmt, i));
			else
				value = cJSON_AddStringToObject(item, key,
					(const char *)sqlite3_column_text(stmt, i));
			if (!value) {
				rc = (-ENOMEM);
				break;
			}
		}
		if (rc != 0 || !cJSON_AddItemToArray(items, item)) {
			cJSON_Delete(item);
			rc = (-ENOMEM);
			break;
		}
	}
	if (rc == 0 && step != SQLITE_DONE)
		rc = (MORPH_ERR_DB);
	sqlite3_finalize(stmt);
	return rc;
}

static int transcript_add_traces(sqlite3 *db, int64_t session_id, cJSON *root)
{
	sqlite3_stmt *stmt = NULL;
	cJSON *items = cJSON_AddArrayToObject(root, "traces");
	int rc = 0;
	int step;

	if (!items)
		return -ENOMEM;
	if (sqlite3_prepare_v2(db,
		"SELECT id,steps_json,created_at FROM react_traces "
		"WHERE session_id=? AND aborted=0 ORDER BY created_at,id",
		-1, &stmt, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		cJSON *item = cJSON_CreateObject();
		const char *steps = (const char *)sqlite3_column_text(stmt, 1);
		if (!item ||
		    !cJSON_AddNumberToObject(item, "id",
			(double)sqlite3_column_int64(stmt, 0)) ||
		    !cJSON_AddStringToObject(item, "steps_json", steps ? steps : "[]") ||
		    !cJSON_AddNumberToObject(item, "created_at",
			(double)sqlite3_column_int64(stmt, 2)) ||
		    !cJSON_AddItemToArray(items, item)) {
			cJSON_Delete(item);
			rc = (-ENOMEM);
			break;
		}
	}
	if (rc == 0 && step != SQLITE_DONE)
		rc = (MORPH_ERR_DB);
	sqlite3_finalize(stmt);
	return rc;
}

int runtime_session_transcript_json(struct runtime *runtime, int64_t session_id,
				    char **out)
{
	struct session session;
	sqlite3_stmt *stmt = NULL;
	cJSON *root = NULL;
	cJSON *items;
	int rc;
	int step;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!runtime || session_id <= 0)
		return -EINVAL;
	rc = session_get_by_id(&runtime->context.database, session_id, &session);
	if (rc != 0)
		return rc;
	root = cJSON_CreateObject();
	items = root ? cJSON_AddArrayToObject(root, "messages") : NULL;
	if (!items || !cJSON_AddStringToObject(root, "identity", session.display_id)) {
		cJSON_Delete(root);
		return -ENOMEM;
	}
	rc = sqlite3_prepare_v2(runtime->context.database.handle,
		"SELECT id,role,content,turn_id,created_at FROM messages "
		"WHERE session_id=? ORDER BY created_at,id", -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		cJSON_Delete(root);
		return MORPH_ERR_DB;
	}
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = 0;
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		cJSON *item = cJSON_CreateObject();
		const char *role = (const char *)sqlite3_column_text(stmt, 1);
		const char *content = (const char *)sqlite3_column_text(stmt, 2);
		const char *turn = (const char *)sqlite3_column_text(stmt, 3);
		if (!item ||
		    !cJSON_AddNumberToObject(item, "id",
			(double)sqlite3_column_int64(stmt, 0)) ||
		    !cJSON_AddStringToObject(item, "role", role ? role : "") ||
		    !cJSON_AddStringToObject(item, "content", content ? content : "") ||
		    !cJSON_AddStringToObject(item, "turn_id", turn ? turn : "") ||
		    !cJSON_AddNumberToObject(item, "created_at",
			(double)sqlite3_column_int64(stmt, 4)) ||
		    !cJSON_AddItemToArray(items, item)) {
			cJSON_Delete(item);
			rc = (-ENOMEM);
			break;
		}
	}
	if (rc == 0 && step != SQLITE_DONE)
		rc = (MORPH_ERR_DB);
	sqlite3_finalize(stmt);
	if (rc == 0)
		rc = transcript_add_display(runtime->context.database.handle, session_id, root);
	if (rc == 0)
		rc = transcript_add_traces(runtime->context.database.handle, session_id, root);
	if (rc == 0) {
		*out = cJSON_PrintUnformatted(root);
		if (!*out)
			rc = (-ENOMEM);
	}
	cJSON_Delete(root);
	return rc;
}

struct model_history_item *runtime_session_model_history_current(
	struct runtime *runtime, int active_only, int *count)
{
	if (!runtime || !count || runtime->context.current_session.id <= 0)
		return NULL;
	return model_history_list(&runtime->context.database,
		runtime->context.current_session.id, active_only, count);
}

void runtime_session_model_history_free(struct model_history_item *items)
{
	model_history_free_list(items);
}

int runtime_session_history_diagnose(struct runtime *runtime,
	struct agent_history_diagnostic *diagnostic)
{
	struct model_history_item *items;
	int count = 0;
	int rc;

	if (!runtime || !diagnostic)
		return -EINVAL;
	items = model_history_list(&runtime->context.database,
		runtime->context.current_session.id, 1, &count);
	if (count < 0)
		return MORPH_ERR_DB;
	rc = agent_history_diagnose(items, runtime->context.tokenizer,
		diagnostic);
	model_history_free_list(items);
	return rc;
}

int runtime_session_history_repair(struct runtime *runtime,
	struct agent_history_diagnostic *before, int *changed)
{
	if (!runtime)
		return -EINVAL;
	return agent_history_repair(&runtime->context.database,
		runtime->context.current_session.id, runtime->context.tokenizer,
		before, changed);
}
