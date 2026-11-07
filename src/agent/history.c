#include "history.h"
#include "history_internal.h"

#include "prompt_context.h"
#include "react.h"
#include "tokenizer.h"
#include "util/cleanup.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/log.h"
#include "util/utf8.h"
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "http/client.h"

int agent_history_record_request(struct react_context *ctx, const struct model *model,
				 const char *system_prompt, const struct chat_message *messages,
				 int count, const struct tool_desc *tools, int tool_count)
{
	cJSON *input = NULL;
	char *json;
	char *safe;
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!ctx || !model)
		return -EINVAL;
	if (!ctx->history_enabled || !ctx->history_db || ctx->history_session_id <= 0)
		return 0;
	rc = model_input_snapshot_create(model, system_prompt, messages, count, tools, tool_count,
					 &input);
	if (rc != 0)
		return rc;
	json = cJSON_PrintUnformatted(input);
	cJSON_Delete(input);
	if (!json)
		return -ENOMEM;
	safe = history_redacted_json_create(ctx, json);
	free(json);
	if (!safe)
		return -ENOMEM;
	rc = sqlite3_prepare_v2(
		ctx->history_db->handle,
		"INSERT INTO model_request_snapshots(session_id,turn_id,provider,model,"
		"input_json,created_at) VALUES(?,?,?,?,?,strftime('%s','now')) "
		"ON CONFLICT(session_id) DO UPDATE SET turn_id=excluded.turn_id,"
		"provider=excluded.provider,model=excluded.model,input_json=excluded.input_json,"
		"created_at=excluded.created_at",
		-1, &stmt, NULL);
	if (rc == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, ctx->history_session_id);
		sqlite3_bind_text(stmt, 2, ctx->turn_id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, model->provider, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, model->model_id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 5, safe, -1, SQLITE_TRANSIENT);
		rc = sqlite3_step(stmt);
	}
	sqlite3_finalize(stmt);
	free(safe);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int history_store(struct react_context *ctx, const struct model_history_insert *insert)
{
	struct model_history_insert safe;
	char *content;
	char *payload;
	int rc;

	if (!ctx || !ctx->history_db || ctx->history_session_id <= 0 || !ctx->history_enabled)
		return 0;
	/* Tool contents are already redacted and token-bounded by prepare_tool_content.
	 * Replacing secrets again here could expand them beyond that budget. */
	content = strcmp(insert->kind, "tool_result") == 0
			  ? (insert->content ? strdup(insert->content) : NULL)
			  : history_redacted_text_create(ctx, insert->content);
	payload = history_redacted_json_create(ctx, insert->payload_json);
	if ((insert->content && !content) || (insert->payload_json && !payload)) {
		free(content);
		free(payload);
		return -ENOMEM;
	}
	safe = *insert;
	safe.content = content;
	safe.payload_json = payload;
	int64_t tokens = tokenizer_count(ctx->tokenizer, content);

	if (strcmp(insert->kind, "assistant_message") == 0 ||
	    strcmp(insert->kind, "assistant_tool_calls") == 0)
		tokens += tokenizer_count(ctx->tokenizer, payload);
	safe.token_count = tokens > INT_MAX ? INT_MAX : (int)tokens;
	rc = model_history_add(ctx->history_db, &safe, NULL);
	free(content);
	free(payload);
	if (rc != 0 && ctx->history_error == 0)
		ctx->history_error = rc;
	return rc;
}

int agent_history_record_user(struct react_context *ctx, const char *content)
{
	char key[160];
	struct model_history_insert item = {
		.session_id = ctx ? ctx->history_session_id : 0,
		.turn_id = ctx ? ctx->turn_id : NULL,
		.kind = "user_message",
		.role = "user",
		.content = content ? content : "",
		.token_count = ctx && ctx->tokenizer
				       ? tokenizer_count(ctx->tokenizer, content ? content : "")
				       : 0,
		.active = 1,
	};

	snprintf(key, sizeof(key), "turn:%s:user",
		 ctx && ctx->turn_id[0] ? ctx->turn_id : "unknown");
	item.idempotency_key = key;

	return history_store(ctx, &item);
}

int agent_history_record_user_steer(struct react_context *ctx, const char *content, int sequence)
{
	char key[192];
	struct model_history_insert item = {
		.session_id = ctx ? ctx->history_session_id : 0,
		.turn_id = ctx ? ctx->turn_id : NULL,
		.kind = "user_message",
		.role = "user",
		.content = content ? content : "",
		.token_count = ctx && ctx->tokenizer
				       ? tokenizer_count(ctx->tokenizer, content ? content : "")
				       : 0,
		.active = 1,
	};

	if (sequence <= 0)
		return -EINVAL;
	snprintf(key, sizeof(key), "turn:%s:user:steer:%d",
		 ctx && ctx->turn_id[0] ? ctx->turn_id : "unknown", sequence);
	item.idempotency_key = key;
	return history_store(ctx, &item);
}

int agent_history_record_assistant(struct react_context *ctx, const char *content)
{
	char key[160];
	struct model_history_insert item = {
		.session_id = ctx ? ctx->history_session_id : 0,
		.turn_id = ctx ? ctx->turn_id : NULL,
		.kind = "assistant_message",
		.role = "assistant",
		.content = content ? content : "",
		.token_count = ctx && ctx->tokenizer
				       ? tokenizer_count(ctx->tokenizer, content ? content : "")
				       : 0,
		.active = 1,
	};

	if (!content || !content[0])
		return 0;
	snprintf(key, sizeof(key), "turn:%s:assistant",
		 ctx && ctx->turn_id[0] ? ctx->turn_id : "unknown");
	item.idempotency_key = key;
	if (ctx && ctx->final_reasoning_content) {
		cJSON *payload = cJSON_CreateObject();
		char *json;
		int rc;

		if (!payload)
			return -ENOMEM;
		if (!cJSON_AddStringToObject(payload, "reasoning_content",
					     ctx->final_reasoning_content)) {
			cJSON_Delete(payload);
			return -ENOMEM;
		}
		json = cJSON_PrintUnformatted(payload);
		cJSON_Delete(payload);
		if (!json)
			return -ENOMEM;
		item.payload_json = json;
		rc = history_store(ctx, &item);
		free(json);
		return rc;
	}
	return history_store(ctx, &item);
}

/* Store lifecycle facts on the request, so compaction cannot separate them. */
int history_store_turn_outcomes(struct db *db, int64_t session_id, const char *turn_id,
				const char *status, int error_code, struct tokenizer *tokenizer,
				int *changed)
{
	const char *select_sql = "SELECT id,content,payload_json "
				 "FROM model_history_items WHERE session_id=? AND turn_id=? "
				 "AND kind='user_message'";
	const char *backfill_sql = "SELECT u.id,u.content,u.payload_json "
				   "FROM model_history_items u WHERE u.session_id=? AND u.active=1 "
				   "AND u.kind='user_message' AND EXISTS (SELECT 1 "
				   "FROM model_history_items a WHERE a.session_id=u.session_id "
				   "AND a.turn_id=u.turn_id AND a.kind='assistant_message' "
				   "AND a.sequence_no>u.sequence_no)";
	const char *update_sql = "UPDATE model_history_items SET "
				 "payload_json=?,token_count=? WHERE id=?";
	sqlite3_stmt *select = NULL;
	sqlite3_stmt *update = NULL;
	int rc;
	int error = 0;

	rc = sqlite3_prepare_v2(db->handle, turn_id ? select_sql : backfill_sql, -1, &select, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_prepare_v2(db->handle, update_sql, -1, &update, NULL);
	if (rc != SQLITE_OK) {
		sqlite3_finalize(select);
		sqlite3_finalize(update);
		return MORPH_ERR_DB;
	}
	sqlite3_bind_int64(select, 1, session_id);
	if (turn_id)
		sqlite3_bind_text(select, 2, turn_id, -1, SQLITE_TRANSIENT);
	while ((rc = sqlite3_step(select)) == SQLITE_ROW) {
		const char *old = (const char *)sqlite3_column_text(select, 2);
		cJSON *payload = old ? cJSON_Parse(old) : cJSON_CreateObject();
		cJSON *outcome = cJSON_CreateObject();
		char *json = NULL;
		char *rendered = NULL;
		struct model_history_item item = {0};

		if (!turn_id && cJSON_HasObjectItem(payload, "turn_outcome")) {
			cJSON_Delete(outcome);
			cJSON_Delete(payload);
			continue;
		}
		if (old && !cJSON_IsObject(payload)) {
			cJSON_Delete(outcome);
			cJSON_Delete(payload);
			error = (MORPH_ERR_PARSE);
			break;
		}
		if (!payload || !outcome || !cJSON_AddStringToObject(outcome, "status", status) ||
		    !cJSON_AddBoolToObject(outcome, "finished", 1) ||
		    !cJSON_AddNumberToObject(outcome, "error_code", error_code)) {
			cJSON_Delete(outcome);
			cJSON_Delete(payload);
			error = (-ENOMEM);
			break;
		}
		cJSON_DeleteItemFromObjectCaseSensitive(payload, "turn_outcome");
		if (!cJSON_AddItemToObject(payload, "turn_outcome", outcome)) {
			cJSON_Delete(outcome);
			cJSON_Delete(payload);
			error = (-ENOMEM);
			break;
		}
		json = cJSON_PrintUnformatted(payload);
		cJSON_Delete(payload);
		item.content = (char *)sqlite3_column_text(select, 1);
		item.payload_json = json;
		if (json)
			rendered = history_user_content_create(&item);
		if (!rendered) {
			free(json);
			error = (-ENOMEM);
			break;
		}
		sqlite3_bind_text(update, 1, json, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(update, 2, tokenizer_count(tokenizer, rendered));
		sqlite3_bind_int64(update, 3, sqlite3_column_int64(select, 0));
		free(rendered);
		free(json);
		if (sqlite3_step(update) != SQLITE_DONE) {
			error = (MORPH_ERR_DB);
			break;
		}
		if (changed)
			(*changed)++;
		sqlite3_reset(update);
		sqlite3_clear_bindings(update);
	}
	sqlite3_finalize(select);
	sqlite3_finalize(update);
	if (error)
		return error;
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	return 0;
}

int agent_history_record_turn_outcome(struct react_context *ctx)
{
	const char *status;

	if (!ctx || !ctx->history_enabled || !ctx->history_db || !ctx->turn_id[0])
		return 0;
	status = react_outcome_name(ctx->outcome);
	if (ctx->outcome == REACT_OUTCOME_NONE)
		status = ctx->state == REACT_STATE_DONE ? "success" : "interrupted";
	return history_store_turn_outcomes(ctx->history_db, ctx->history_session_id, ctx->turn_id,
					   status, ctx->last_error_code, ctx->tokenizer, NULL);
}

int agent_history_record_tool_calls(struct react_context *ctx, const char *content,
				    const char *reasoning_content, const struct tool_call *calls,
				    int call_count)
{
	struct model_history_insert item = {0};
	char key[256];
	cJSON *array;
	cJSON *root;
	char *json;
	int rc;

	if (!ctx || !calls || call_count <= 0)
		return -EINVAL;
	root = cJSON_CreateObject();
	array = cJSON_CreateArray();
	if (!root || !array) {
		cJSON_Delete(root);
		cJSON_Delete(array);
		return -ENOMEM;
	}
	cJSON_AddItemToObject(root, "calls", array);
	if (reasoning_content)
		cJSON_AddStringToObject(root, "reasoning_content", reasoning_content);
	for (int i = 0; i < call_count; i++) {
		cJSON *call = cJSON_CreateObject();
		char *arguments = NULL;

		if (!call) {
			cJSON_Delete(root);
			return -ENOMEM;
		}
		cJSON_AddStringToObject(call, "tool_call_id", calls[i].tool_call_id);
		cJSON_AddStringToObject(call, "provider_call_id", calls[i].id);
		cJSON_AddStringToObject(call, "name", calls[i].name);
		if (calls[i].input_kind == TOOL_INPUT_TEXT) {
			cJSON_AddStringToObject(call, "input_kind", "text");
			cJSON_AddStringToObject(call, "input",
						calls[i].arguments ? calls[i].arguments : "");
		} else {
			rc = agent_history_tool_arguments_copy(calls[i].arguments, &arguments);
			if (rc != 0) {
				cJSON_Delete(call);
				cJSON_Delete(root);
				return rc;
			}
			cJSON_AddStringToObject(call, "input_kind", "json");
			cJSON_AddStringToObject(call, "arguments", arguments);
		}
		free(arguments);
		cJSON_AddItemToArray(array, call);
	}
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json)
		return -ENOMEM;
	item.session_id = ctx->history_session_id;
	item.turn_id = ctx->turn_id;
	item.kind = "assistant_tool_calls";
	item.role = "assistant";
	item.content = content;
	item.payload_json = json;
	snprintf(key, sizeof(key), "calls:%s", calls[0].tool_call_id);
	item.idempotency_key = key;
	item.token_count = tokenizer_count(ctx->tokenizer, json);
	item.active = 1;
	rc = history_store(ctx, &item);
	free(json);
	return rc;
}

/* Keep useful head/tail context, then verify the whole stored string, including
 * its marker, with the same tokenizer used for the active request. */
int agent_history_record_tool_result_ex(struct react_context *ctx, const char *tool_call_id,
					const char *provider_call_id, const char *tool_name,
					const char *content, int error_code,
					const struct tool_artifact_list *artifacts,
					const cJSON *tool_meta)
{
	struct model_history_insert item = {0};
	char key[256];
	cJSON *metadata;
	char *payload = NULL;
	char *stored = NULL;
	int truncated = 0;
	int rc;

	if (!ctx)
		return -EINVAL;
	rc = agent_history_tool_content_copy(ctx, content, &stored, &truncated);
	if (rc != 0)
		return rc;
	item.session_id = ctx->history_session_id;
	item.turn_id = ctx->turn_id;
	item.kind = "tool_result";
	item.role = "tool";
	item.content = stored;
	item.tool_call_id = tool_call_id;
	item.provider_call_id = provider_call_id;
	item.tool_name = tool_name;
	snprintf(key, sizeof(key), "result:%s", tool_call_id ? tool_call_id : "unknown");
	item.idempotency_key = key;
	metadata = cJSON_CreateObject();
	if (!metadata) {
		free(stored);
		return -ENOMEM;
	}
	cJSON_AddStringToObject(metadata, "status",
				error_code == -ETIMEDOUT   ? "timed_out"
				: error_code == -ECANCELED ? "cancelled"
				: error_code < 0	   ? "failed"
							   : "completed");
	cJSON_AddNumberToObject(metadata, "error_code", error_code);
	cJSON_AddStringToObject(metadata, "tool_call_id", tool_call_id ? tool_call_id : "");
	cJSON_AddStringToObject(metadata, "provider_call_id",
				provider_call_id ? provider_call_id : "");
	cJSON_AddStringToObject(metadata, "tool_name", tool_name ? tool_name : "");
	cJSON_AddStringToObject(metadata, "content", stored);
	{
		char *original = strdup(content ? content : "");
		char *redacted;

		if (!original) {
			cJSON_Delete(metadata);
			free(stored);
			return -ENOMEM;
		}
		utf8_sanitize_inplace(original);
		redacted = history_redacted_json_create(ctx, original);
		free(original);
		if (!redacted || !cJSON_AddStringToObject(metadata, "original_content", redacted)) {
			free(redacted);
			cJSON_Delete(metadata);
			free(stored);
			return -ENOMEM;
		}
		free(redacted);
	}
	if (artifacts && artifacts->count > 0) {
		cJSON *artifact_json = tool_artifact_list_json_create(artifacts);

		if (artifact_json)
			cJSON_AddItemToObject(metadata, "artifacts", artifact_json);
	}
	if (tool_meta)
		cJSON_AddItemToObject(metadata, "tool_meta", cJSON_Duplicate(tool_meta, 1));
	{
		cJSON *storage = cJSON_CreateObject();

		if (storage) {
			cJSON_AddBoolToObject(storage, "truncated", truncated);
			cJSON_AddNumberToObject(storage, "original_bytes",
						content ? (double)strlen(content) : 0.0);
			cJSON_AddNumberToObject(storage, "stored_bytes", (double)strlen(stored));
			cJSON_AddItemToObject(metadata, "storage", storage);
		}
	}
	payload = cJSON_PrintUnformatted(metadata);
	cJSON_Delete(metadata);
	if (!payload) {
		free(stored);
		return -ENOMEM;
	}
	item.payload_json = payload;
	item.token_count = tokenizer_count(ctx->tokenizer, stored);
	item.truncated = truncated;
	item.active = 1;
	rc = history_store(ctx, &item);
	free(payload);
	free(stored);
	return rc;
}

int agent_history_record_tool_result(struct react_context *ctx, const char *tool_call_id,
				     const char *provider_call_id, const char *tool_name,
				     const char *content, int error_code)
{
	return agent_history_record_tool_result_ex(ctx, tool_call_id, provider_call_id, tool_name,
						   content, error_code, NULL, NULL);
}

int agent_history_record_receipt(struct react_context *ctx, const char *name, const char *phase,
				 const char *message, const char *task, int count, int error_code)
{
	struct model_history_insert item = {0};
	morph_buf_t text MORPH_CLEANUP(morph_buf_cleanup) = {0};
	cJSON *payload;
	char *json;
	char key[320];
	int rc;

	if (!ctx)
		return -EINVAL;
	/* Memory maintenance is UI/log telemetry, not conversation evidence. */
	if (task && strcmp(task, "memory_consolidation") == 0)
		return 0;
	if (!ctx->history_enabled || !ctx->history_db)
		return 0;
	rc = morph_buf_init(&text, 256);
	if (rc != 0)
		return rc;
	rc = morph_buf_printf(&text, "[Background receipt] %s: %s",
			      task ? task : (name ? name : "task"),
			      message ? message : (phase ? phase : "updated"));
	if (rc != 0) {
		morph_buf_cleanup(&text);
		return rc;
	}
	payload = cJSON_CreateObject();
	if (!payload) {
		morph_buf_cleanup(&text);
		return -ENOMEM;
	}
	cJSON_AddStringToObject(payload, "name", name ? name : "");
	cJSON_AddStringToObject(payload, "phase", phase ? phase : "");
	cJSON_AddStringToObject(payload, "task", task ? task : "");
	cJSON_AddNumberToObject(payload, "count", count);
	cJSON_AddNumberToObject(payload, "error_code", error_code);
	json = cJSON_PrintUnformatted(payload);
	cJSON_Delete(payload);
	if (!json) {
		morph_buf_cleanup(&text);
		return -ENOMEM;
	}
	snprintf(key, sizeof(key), "receipt:%s:%s:%s:%d",
		 ctx->turn_id[0] ? ctx->turn_id : "unknown", name ? name : "", phase ? phase : "",
		 count);
	item.session_id = ctx->history_session_id;
	item.turn_id = ctx->turn_id;
	item.kind = "background_receipt";
	item.role = "system";
	item.content = morph_buf_cstr_borrow(&text);
	item.payload_json = json;
	item.idempotency_key = key;
	item.token_count = tokenizer_count(ctx->tokenizer, item.content);
	item.active = 1;
	rc = history_store(ctx, &item);
	free(json);
	morph_buf_cleanup(&text);
	return rc;
}
