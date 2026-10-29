#include "prompt_context.h"
#include "history.h"
#include "react.h"
#include "tokenizer.h"
#include "util/arena.h"
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

#define HISTORY_DEFAULT_TOOL_RESULT_MAX_TOKENS 8000

static struct chat_message *history_push_message(morph_array_t *messages)
{
	struct chat_message *message = morph_array_push(messages);

	if (message)
		memset(message, 0, sizeof(*message));
	return message;
}

static int history_secret_prefix(const char *text, size_t *prefix_len,
				 int *preserve_prefix)
{
	static const char *const prefixes[] = {
		"Bearer ", "sk-", "sk_live_", "sk_test_",
		"api_key=", "api_key:", "\"api_key\":\"",
		"\"apiKey\":\"", "OPENAI_API_KEY="
	};

	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		size_t len = strlen(prefixes[i]);

		if (strncmp(text, prefixes[i], len) == 0) {
			*prefix_len = len;
			*preserve_prefix = i == 0 || i >= 4;
			return 1;
		}
	}
	return 0;
}

static char *history_redact(const char *text)
{
	morph_buf_t buf;
	const char *cur;
	int rc;

	if (!text)
		return NULL;
	rc = morph_buf_init(&buf, strlen(text) + 1);
	if (rc != 0)
		return NULL;
	cur = text;
	while (*cur) {
		size_t prefix_len = 0;
		int preserve_prefix = 0;
		if (strncmp(cur, "data:", 5) == 0) {
			const char *base64 = strstr(cur, ";base64,");
			const char *header = cur + 5;

			while (*header && (isalnum((unsigned char)*header) ||
			       strchr("/;=.+-", *header)))
				header++;
			if (base64 && (size_t)(base64 - cur) < 128 &&
			    header == base64 + strlen(";base64")) {
				rc = morph_buf_puts(&buf, "[binary data omitted]");
				cur = base64 + strlen(";base64,");
				while (*cur && (isalnum((unsigned char)*cur) ||
				       strchr("+/=", *cur)))
					cur++;
				if (rc != 0)
					break;
				continue;
			}
		}

		if (!history_secret_prefix(cur, &prefix_len,
					   &preserve_prefix)) {
			rc = morph_buf_putc(&buf, *cur++);
			if (rc != 0)
				break;
			continue;
		}
		if (preserve_prefix)
			rc = morph_buf_append(&buf, cur, prefix_len);
		if (rc == 0)
			rc = morph_buf_puts(&buf, "[REDACTED]");
		cur += prefix_len;
		while (*cur && isspace((unsigned char)*cur))
			cur++;
		while (*cur && !isspace((unsigned char)*cur) &&
		       *cur != '"' && *cur != '\'' && *cur != ',' &&
		       *cur != '}')
			cur++;
		if (rc != 0)
			break;
	}
	if (rc != 0) {
		morph_buf_cleanup(&buf);
		return NULL;
	}
	return morph_buf_detach(&buf);
}

static char *history_redact_exact(const char *text, const char *secret)
{
	morph_buf_t buf;
	const char *cur;
	const char *match;
	int rc;

	if (!text)
		return NULL;
	if (!secret || strlen(secret) < 4)
		return strdup(text);
	rc = morph_buf_init(&buf, strlen(text) + 1);
	if (rc != 0)
		return NULL;
	cur = text;
	while ((match = strstr(cur, secret)) != NULL) {
		rc = morph_buf_append(&buf, cur, (size_t)(match - cur));
		if (rc == 0)
			rc = morph_buf_puts(&buf, "[REDACTED]");
		if (rc != 0) {
			morph_buf_cleanup(&buf);
			return NULL;
		}
		cur = match + strlen(secret);
	}
	if (morph_buf_puts(&buf, cur) != 0) {
		morph_buf_cleanup(&buf);
		return NULL;
	}
	return morph_buf_detach(&buf);
}

static char *history_redact_all(struct react_context *ctx, const char *text)
{
	char *safe = history_redact(text);

	if (!safe)
		return NULL;
	for (int i = 0; ctx && i < ctx->history_secret_count; i++) {
		char *next = history_redact_exact(safe, ctx->history_secrets[i]);

		free(safe);
		safe = next;
		if (!safe)
			return NULL;
	}
	return safe;
}

/* Redact decoded values so replacements cannot corrupt JSON escaping.
 * Tool arguments may themselves contain a serialized JSON object. */
static char *history_redact_json(struct react_context *ctx, const char *text);

static int history_redact_json_values(struct react_context *ctx, cJSON *value)
{
	if (cJSON_IsString(value)) {
		char *safe = history_redact_json(ctx, value->valuestring);
		int rc = safe && cJSON_SetValuestring(value, safe) ? 0 : -ENOMEM;

		free(safe);
		return rc;
	}
	for (cJSON *child = value->child; child; child = child->next) {
		int rc = history_redact_json_values(ctx, child);

		if (rc != 0)
			return rc;
	}
	return 0;
}

static char *history_redact_json(struct react_context *ctx, const char *text)
{
	cJSON *root;
	char *safe = NULL;

	if (!text)
		return NULL;
	root = cJSON_ParseWithOpts(text, NULL, 1);
	if (!cJSON_IsObject(root) && !cJSON_IsArray(root)) {
		cJSON_Delete(root);
		return history_redact_all(ctx, text);
	}
	if (history_redact_json_values(ctx, root) == 0)
		safe = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return safe;
}

static int history_add_text_message(morph_array_t *messages,
				    struct arena *arena,
				    const char *role,
				    const char *content)
{
	struct chat_message *message = history_push_message(messages);

	if (!message)
		MORPH_RETURN(-ENOMEM);
	message->role = arena_strdup(arena, role ? role : "user");
	message->content = arena_strdup(arena, content ? content : "");
	if (!message->role || !message->content)
		MORPH_RETURN(-ENOMEM);
	return 0;
}

int agent_history_record_request(struct react_context *ctx, const struct model *model,
	const char *system_prompt, const struct chat_message *messages, int count,
	const struct tool_desc *tools, int tool_count)
{
	cJSON *input = NULL;
	char *json;
	char *safe;
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!ctx || !model)
		MORPH_RETURN(-EINVAL);
	if (!ctx->history_enabled || !ctx->history_db || ctx->history_session_id <= 0)
		return 0;
	rc = model_input_snapshot(model, system_prompt, messages, count, tools, tool_count, &input);
	if (rc != 0)
		return rc;
	json = cJSON_PrintUnformatted(input);
	cJSON_Delete(input);
	if (!json)
		MORPH_RETURN(-ENOMEM);
	safe = history_redact_json(ctx, json);
	free(json);
	if (!safe)
		MORPH_RETURN(-ENOMEM);
	rc = sqlite3_prepare_v2(ctx->history_db->handle,
		"INSERT INTO model_request_snapshots(session_id,turn_id,provider,model,"
		"input_json,created_at) VALUES(?,?,?,?,?,strftime('%s','now')) "
		"ON CONFLICT(session_id) DO UPDATE SET turn_id=excluded.turn_id,"
		"provider=excluded.provider,model=excluded.model,input_json=excluded.input_json,"
		"created_at=excluded.created_at", -1, &stmt, NULL);
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
	MORPH_RETURN(rc == SQLITE_DONE ? 0 : MORPH_ERR_DB);
}

static int history_tool_arguments_valid(const cJSON *arguments)
{
	cJSON *parsed;
	int valid;

	if (!cJSON_IsString(arguments) || !arguments->valuestring)
		return 0;
	parsed = cJSON_ParseWithOpts(arguments->valuestring, NULL, 1);
	valid = cJSON_IsObject(parsed);
	cJSON_Delete(parsed);
	return valid;
}

static int history_tool_call_input(const cJSON *call,
				   enum tool_input_kind *kind,
				   const char **input)
{
	cJSON *kind_item;
	cJSON *value;

	if (!cJSON_IsObject(call) || !kind || !input)
		return 0;
	kind_item = cJSON_GetObjectItem(call, "input_kind");
	if (cJSON_IsString(kind_item) && kind_item->valuestring &&
	    strcmp(kind_item->valuestring, "text") == 0) {
		value = cJSON_GetObjectItem(call, "input");
		if (!cJSON_IsString(value) || !value->valuestring)
			return 0;
		*kind = TOOL_INPUT_TEXT;
		*input = value->valuestring;
		return 1;
	}
	value = cJSON_GetObjectItem(call, "arguments");
	if (!history_tool_arguments_valid(value))
		return 0;
	*kind = TOOL_INPUT_JSON;
	*input = value->valuestring;
	return 1;
}

int agent_history_normalize_tool_arguments(const char *arguments,
					   char **normalized)
{
	const char *raw = arguments ? arguments : "{}";
	cJSON *parsed;
	cJSON *fallback;
	char *json;

	if (!normalized)
		MORPH_RETURN(-EINVAL);
	*normalized = NULL;
	parsed = cJSON_ParseWithOpts(raw, NULL, 1);
	if (cJSON_IsObject(parsed)) {
		cJSON_Delete(parsed);
		*normalized = strdup(raw);
		if (!*normalized)
			MORPH_RETURN(-ENOMEM);
		return 0;
	}
	cJSON_Delete(parsed);
	fallback = cJSON_CreateObject();
	if (!fallback)
		MORPH_RETURN(-ENOMEM);
	cJSON_AddStringToObject(fallback, "_morph_invalid_arguments", raw);
	cJSON_AddStringToObject(fallback, "_morph_recovery",
		"Reissue this tool call with a valid JSON object.");
	json = cJSON_PrintUnformatted(fallback);
	cJSON_Delete(fallback);
	if (!json)
		MORPH_RETURN(-ENOMEM);
	*normalized = json;
	return 0;
}

int agent_history_migrate_legacy_tool_call(struct tool_call *call,
					   struct arena *arena)
{
	cJSON *arguments;
	char *normalized;

	if (!call || strcmp(call->name, "bash_exec") != 0)
		return 0;
	snprintf(call->name, sizeof(call->name), "%s", "exec");
	if (call->input_kind != TOOL_INPUT_JSON || !call->arguments)
		return 0;
	arguments = cJSON_Parse(call->arguments);
	if (!cJSON_IsObject(arguments)) {
		cJSON_Delete(arguments);
		return 0;
	}
	cJSON_DeleteItemFromObject(arguments, "timeout_seconds");
	cJSON_DeleteItemFromObject(arguments, "cwd");
	cJSON_DeleteItemFromObject(arguments, "write_paths");
	normalized = cJSON_PrintUnformatted(arguments);
	cJSON_Delete(arguments);
	if (!normalized)
		MORPH_RETURN(-ENOMEM);
	if (arena) {
		char *copy = arena_strdup(arena, normalized);

		free(normalized);
		if (!copy)
			MORPH_RETURN(-ENOMEM);
		call->arguments = copy;
	} else {
		free(call->arguments);
		call->arguments = normalized;
	}
	return 0;
}

static int history_add_tool_calls(struct morph_log *logger, const struct model_history_item *item,
				  morph_array_t *messages, struct arena *arena)
{
	struct chat_message *message;
	cJSON *calls;
	cJSON *root;
	int count;

	root = item->payload_json ? cJSON_ParseWithOpts(item->payload_json, NULL, 1) : NULL;
	if (!root) {
		log_err(logger, "history replay: invalid tool payload at item %lld, sequence %lld",
			(long long)item->id, (long long)item->sequence_no);
		MORPH_RETURN(MORPH_ERR_PARSE);
	}
	calls = cJSON_GetObjectItem(root, "calls");
	count = cJSON_IsArray(calls) ? cJSON_GetArraySize(calls) : 0;
	if (count <= 0) {
		cJSON_Delete(root);
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	}
	message = history_push_message(messages);
	if (!message) {
		cJSON_Delete(root);
		MORPH_RETURN(-ENOMEM);
	}
	message->role = arena_strdup(arena, "assistant");
	message->content = item->content && item->content[0] ?
		arena_strdup(arena, item->content) : NULL;
	cJSON *reasoning = cJSON_GetObjectItem(root, "reasoning_content");
	if (cJSON_IsString(reasoning) && reasoning->valuestring)
		message->reasoning_content = arena_strdup(arena,
			reasoning->valuestring);
	message->tool_calls = arena_alloc(arena,
		(size_t)count * sizeof(*message->tool_calls));
	if (!message->role || !message->tool_calls ||
	    (cJSON_IsString(reasoning) && !message->reasoning_content)) {
		cJSON_Delete(root);
		MORPH_RETURN(-ENOMEM);
	}
	memset(message->tool_calls, 0,
	       (size_t)count * sizeof(*message->tool_calls));
	message->tool_call_count = count;
	for (int i = 0; i < count; i++) {
		cJSON *call = cJSON_GetArrayItem(calls, i);
		cJSON *provider_id = cJSON_GetObjectItem(call,
							"provider_call_id");
		cJSON *local_id = cJSON_GetObjectItem(call, "tool_call_id");
		cJSON *name = cJSON_GetObjectItem(call, "name");
		struct tool_call *tool_call = &message->tool_calls[i];
		const char *input;
		enum tool_input_kind input_kind;

		if (!cJSON_IsString(provider_id) ||
		    !cJSON_IsString(local_id) || !cJSON_IsString(name) ||
		    !history_tool_call_input(call, &input_kind, &input)) {
			cJSON_Delete(root);
			MORPH_RETURN(MORPH_ERR_PROTOCOL);
		}
		/* Replayed IDs are provider-neutral Morph IDs. */
		snprintf(tool_call->id, sizeof(tool_call->id), "%s",
			 local_id->valuestring);
		snprintf(tool_call->tool_call_id,
			 sizeof(tool_call->tool_call_id), "%s",
			 local_id->valuestring);
		snprintf(tool_call->name, sizeof(tool_call->name), "%s",
			 name->valuestring);
		tool_call->input_kind = input_kind;
		tool_call->arguments = arena_strdup(arena, input);
		if (!tool_call->arguments) {
			cJSON_Delete(root);
			MORPH_RETURN(-ENOMEM);
		}
		int migrate_rc = agent_history_migrate_legacy_tool_call(
			tool_call, arena);

		if (migrate_rc != 0) {
			cJSON_Delete(root);
			return migrate_rc;
		}
	}
	cJSON_Delete(root);
	return 0;
}

static int history_add_tool_result(const struct model_history_item *item,
				   morph_array_t *messages,
				   struct arena *arena)
{
	struct chat_message *message = history_push_message(messages);
	const char *call_id = item->tool_call_id ?
		item->tool_call_id : item->provider_call_id;

	if (!message)
		MORPH_RETURN(-ENOMEM);
	message->role = arena_strdup(arena, "tool");
	message->content = arena_strdup(arena,
		item->content ? item->content : "");
	message->tool_call_id = arena_strdup(arena, call_id ? call_id : "");
	if (!message->role || !message->content || !message->tool_call_id)
		MORPH_RETURN(-ENOMEM);
	return 0;
}

static int history_add_assistant_message(const struct model_history_item *item,
	morph_array_t *messages, struct arena *arena)
{
	int rc = history_add_text_message(messages, arena, "assistant", item->content);
	cJSON *payload;
	const cJSON *reasoning;
	struct chat_message *message;

	if (rc != 0 || !item->payload_json || !item->payload_json[0])
		return rc;
	payload = cJSON_ParseWithOpts(item->payload_json, NULL, 1);
	if (!payload)
		MORPH_RETURN(MORPH_ERR_PARSE);
	reasoning = cJSON_GetObjectItemCaseSensitive(payload, "reasoning_content");
	message = morph_array_get(messages, messages->nelts - 1);
	if (cJSON_IsString(reasoning)) {
		message->reasoning_content = arena_strdup(arena, reasoning->valuestring);
		if (!message->reasoning_content)
			MORPH_SET_ERR(rc, -ENOMEM);
	}
	cJSON_Delete(payload);
	return rc;
}

static char *history_user_content(const struct model_history_item *item)
{
	cJSON *payload = item->payload_json ? cJSON_Parse(item->payload_json) : NULL;
	const cJSON *outcome = cJSON_GetObjectItemCaseSensitive(payload, "turn_outcome");
	const cJSON *status = cJSON_GetObjectItemCaseSensitive(outcome, "status");
	const cJSON *finished = cJSON_GetObjectItemCaseSensitive(outcome, "finished");
	morph_buf_t text;
	int rc = morph_buf_init(&text, 256);

	if (rc == 0)
		rc = morph_buf_puts(&text, item->content ? item->content : "");
	if (rc == 0 && cJSON_IsString(status) && cJSON_IsTrue(finished))
		rc = morph_buf_printf(&text,
			"\n\n[Historical turn execution ended: %s. This records "
			"execution status, not completion of a broader objective. "
			"Later results supersede earlier checkpoint TODOs for this turn.]",
			status->valuestring);
	cJSON_Delete(payload);
	if (rc != 0) {
		morph_buf_cleanup(&text);
		return NULL;
	}
	return morph_buf_detach(&text);
}

static int history_item_tokens(const struct model_history_item *item,
	struct tokenizer *tokenizer, const char *text)
{
	if (strcmp(item->kind, "user_message") == 0) {
		char *rendered = history_user_content(item);
		int tokens;

		if (!rendered)
			MORPH_RETURN(-ENOMEM);
		tokens = tokenizer_count(tokenizer, rendered);
		free(rendered);
		return tokens;
	}
	int64_t tokens = tokenizer_count(tokenizer, text ? text : "");

	if (strcmp(item->kind, "assistant_tool_calls") == 0)
		tokens += tokenizer_count(tokenizer, item->content);
	if (strcmp(item->kind, "assistant_message") == 0 && item->payload_json)
		tokens += tokenizer_count(tokenizer, item->payload_json);
	return tokens > INT_MAX ? INT_MAX : (int)tokens;
}

int agent_history_item_replayable(const struct model_history_item *item)
{
	if (!item)
		return 0;
	if (strcmp(item->kind, "background_receipt") == 0) {
		static const char prefix[] = "[Background receipt] memory_consolidation:";
		cJSON *payload = item->payload_json ? cJSON_Parse(item->payload_json) : NULL;
		const cJSON *task = cJSON_GetObjectItemCaseSensitive(payload, "task");
		int internal = (cJSON_IsString(task) &&
			strcmp(task->valuestring, "memory_consolidation") == 0) ||
			(item->content && strncmp(item->content, prefix, sizeof(prefix) - 1) == 0);

		cJSON_Delete(payload);
		return !internal;
	}
	return strcmp(item->kind, "user_message") == 0 ||
		strcmp(item->kind, "assistant_message") == 0 ||
		strcmp(item->kind, "assistant_tool_calls") == 0 ||
		strcmp(item->kind, "tool_result") == 0 ||
		strcmp(item->kind, "compaction_summary") == 0;
}

int agent_history_build_chat_messages(struct morph_log *logger,
				      const struct model_history_item *items,
				      morph_array_t *messages, struct arena *arena)
{
	const struct model_history_item *item = items;
	int rc;

	if (!messages || !arena)
		MORPH_RETURN(-EINVAL);
	while (item) {
		if (strcmp(item->kind, "compaction_summary") == 0) {
			morph_buf_t snapshot;

			rc = morph_buf_init_arena(&snapshot, arena, 256);
			if (rc == 0)
				rc = morph_buf_printf(&snapshot,
					"Snapshot through history sequence %lld. Later "
					"messages and turn outcomes supersede this snapshot; "
					"its pending work may already be resolved below.\n%s",
					(long long)(item->sequence_no > 0 ?
					 item->sequence_no - 1 : 0),
					item->content ? item->content : "");
			if (rc != 0)
				return rc;
			char *reference = prompt_reference_build(arena,
				morph_buf_cstr(&snapshot));

			if (!reference)
				MORPH_RETURN(-ENOMEM);
			rc = history_add_text_message(messages, arena, "assistant",
				reference);
			if (rc != 0)
				return rc;
			struct chat_message *message = morph_array_get(messages,
				messages->nelts - 1);

			message->is_context_summary = 1;
		}
		item = item->next;
	}
	item = items;
	while (item) {
		if (!agent_history_item_replayable(item)) {
			item = item->next;
			continue;
		}
		if (strcmp(item->kind, "assistant_tool_calls") == 0)
			rc = history_add_tool_calls(logger, item, messages, arena);
		else if (strcmp(item->kind, "tool_result") == 0)
			rc = history_add_tool_result(item, messages, arena);
		else if (strcmp(item->kind, "compaction_summary") == 0)
			rc = 0;
		else if (strcmp(item->kind, "user_message") == 0) {
			char *content = history_user_content(item);

			if (!content)
				MORPH_RETURN(-ENOMEM);
			rc = history_add_text_message(messages, arena, "user",
				content);
			free(content);
		} else if (strcmp(item->kind, "assistant_message") == 0)
			rc = history_add_assistant_message(item, messages, arena);
		else if (strcmp(item->kind, "background_receipt") == 0)
			rc = history_add_text_message(messages, arena, "system",
				item->content);
		else
			rc = 0;
		if (rc != 0)
			return rc;
		item = item->next;
	}
	return 0;
}

static int history_store(struct react_context *ctx,
			 const struct model_history_insert *insert)
{
	struct model_history_insert safe;
	char *content;
	char *payload;
	int rc;

	if (!ctx || !ctx->history_db || ctx->history_session_id <= 0 ||
	    !ctx->history_enabled)
		return 0;
	/* Tool contents are already redacted and token-bounded by prepare_tool_content.
	 * Replacing secrets again here could expand them beyond that budget. */
	content = strcmp(insert->kind, "tool_result") == 0 ?
		(insert->content ? strdup(insert->content) : NULL) :
		history_redact_all(ctx, insert->content);
	payload = history_redact_json(ctx, insert->payload_json);
	if ((insert->content && !content) || (insert->payload_json && !payload)) {
		free(content);
		free(payload);
		MORPH_RETURN(-ENOMEM);
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
		.token_count = ctx && ctx->tokenizer ?
			tokenizer_count(ctx->tokenizer, content ? content : "") : 0,
		.active = 1,
	};

	snprintf(key, sizeof(key), "turn:%s:user",
		ctx && ctx->turn_id[0] ? ctx->turn_id : "unknown");
	item.idempotency_key = key;

	return history_store(ctx, &item);
}

int agent_history_record_user_steer(struct react_context *ctx,
				    const char *content, int sequence)
{
	char key[192];
	struct model_history_insert item = {
		.session_id = ctx ? ctx->history_session_id : 0,
		.turn_id = ctx ? ctx->turn_id : NULL,
		.kind = "user_message",
		.role = "user",
		.content = content ? content : "",
		.token_count = ctx && ctx->tokenizer ?
			tokenizer_count(ctx->tokenizer, content ? content : "") : 0,
		.active = 1,
	};

	if (sequence <= 0)
		return -EINVAL;
	snprintf(key, sizeof(key), "turn:%s:user:steer:%d",
		ctx && ctx->turn_id[0] ? ctx->turn_id : "unknown", sequence);
	item.idempotency_key = key;
	return history_store(ctx, &item);
}

int agent_history_record_assistant(struct react_context *ctx,
				   const char *content)
{
	char key[160];
	struct model_history_insert item = {
		.session_id = ctx ? ctx->history_session_id : 0,
		.turn_id = ctx ? ctx->turn_id : NULL,
		.kind = "assistant_message",
		.role = "assistant",
		.content = content ? content : "",
		.token_count = ctx && ctx->tokenizer ?
			tokenizer_count(ctx->tokenizer, content ? content : "") : 0,
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
			MORPH_RETURN(-ENOMEM);
		if (!cJSON_AddStringToObject(payload, "reasoning_content",
			ctx->final_reasoning_content)) {
			cJSON_Delete(payload);
			MORPH_RETURN(-ENOMEM);
		}
		json = cJSON_PrintUnformatted(payload);
		cJSON_Delete(payload);
		if (!json)
			MORPH_RETURN(-ENOMEM);
		item.payload_json = json;
		rc = history_store(ctx, &item);
		free(json);
		return rc;
	}
	return history_store(ctx, &item);
}

/* Store lifecycle facts on the request, so compaction cannot separate them. */
static int history_store_turn_outcomes(struct db *db, int64_t session_id,
	const char *turn_id, const char *status, int error_code,
	struct tokenizer *tokenizer, int *changed)
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

	rc = sqlite3_prepare_v2(db->handle, turn_id ? select_sql : backfill_sql,
		-1, &select, NULL);
	if (rc == SQLITE_OK)
		rc = sqlite3_prepare_v2(db->handle, update_sql, -1,
			&update, NULL);
	if (rc != SQLITE_OK) {
		sqlite3_finalize(select);
		sqlite3_finalize(update);
		MORPH_RETURN(MORPH_ERR_DB);
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
			MORPH_SET_ERR(error, MORPH_ERR_PARSE);
			break;
		}
		if (!payload || !outcome ||
		    !cJSON_AddStringToObject(outcome, "status", status) ||
		    !cJSON_AddBoolToObject(outcome, "finished", 1) ||
		    !cJSON_AddNumberToObject(outcome, "error_code", error_code)) {
			cJSON_Delete(outcome);
			cJSON_Delete(payload);
			MORPH_SET_ERR(error, -ENOMEM);
			break;
		}
		cJSON_DeleteItemFromObjectCaseSensitive(payload, "turn_outcome");
		if (!cJSON_AddItemToObject(payload, "turn_outcome", outcome)) {
			cJSON_Delete(outcome);
			cJSON_Delete(payload);
			MORPH_SET_ERR(error, -ENOMEM);
			break;
		}
		json = cJSON_PrintUnformatted(payload);
		cJSON_Delete(payload);
		item.content = (char *)sqlite3_column_text(select, 1);
		item.payload_json = json;
		if (json)
			rendered = history_user_content(&item);
		if (!rendered) {
			free(json);
			MORPH_SET_ERR(error, -ENOMEM);
			break;
		}
		sqlite3_bind_text(update, 1, json, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(update, 2, tokenizer_count(tokenizer, rendered));
		sqlite3_bind_int64(update, 3, sqlite3_column_int64(select, 0));
		free(rendered);
		free(json);
		if (sqlite3_step(update) != SQLITE_DONE) {
			MORPH_SET_ERR(error, MORPH_ERR_DB);
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
		MORPH_RETURN(MORPH_ERR_DB);
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
	return history_store_turn_outcomes(ctx->history_db,
		ctx->history_session_id, ctx->turn_id, status,
		ctx->last_error_code, ctx->tokenizer, NULL);
}

int agent_history_record_tool_calls(struct react_context *ctx,
				    const char *content,
				    const char *reasoning_content,
				    const struct tool_call *calls,
				    int call_count)
{
	struct model_history_insert item = {0};
	char key[256];
	cJSON *array;
	cJSON *root;
	char *json;
	int rc;

	if (!ctx || !calls || call_count <= 0)
		MORPH_RETURN(-EINVAL);
	root = cJSON_CreateObject();
	array = cJSON_CreateArray();
	if (!root || !array) {
		cJSON_Delete(root);
		cJSON_Delete(array);
		MORPH_RETURN(-ENOMEM);
	}
	cJSON_AddItemToObject(root, "calls", array);
	if (reasoning_content)
		cJSON_AddStringToObject(root, "reasoning_content",
			reasoning_content);
	for (int i = 0; i < call_count; i++) {
		cJSON *call = cJSON_CreateObject();
		char *arguments = NULL;

		if (!call) {
			cJSON_Delete(root);
			MORPH_RETURN(-ENOMEM);
		}
		cJSON_AddStringToObject(call, "tool_call_id",
			calls[i].tool_call_id);
		cJSON_AddStringToObject(call, "provider_call_id", calls[i].id);
		cJSON_AddStringToObject(call, "name", calls[i].name);
		if (calls[i].input_kind == TOOL_INPUT_TEXT) {
			cJSON_AddStringToObject(call, "input_kind", "text");
			cJSON_AddStringToObject(call, "input",
				calls[i].arguments ? calls[i].arguments : "");
		} else {
			rc = agent_history_normalize_tool_arguments(
				calls[i].arguments, &arguments);
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
		MORPH_RETURN(-ENOMEM);
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
static char *history_truncate_result(struct tokenizer *tokenizer,
	const char *content, int max_tokens, int *truncated)
{
	const char *marker = "\n…(tool output truncated)…\n";
	morph_buf_t buf;
	size_t len = strlen(content);
	size_t budget;
	int tokens;
	int rc;

	if (max_tokens <= 0)
		max_tokens = HISTORY_DEFAULT_TOOL_RESULT_MAX_TOKENS;
	tokens = tokenizer_count(tokenizer, content);
	if (tokens <= max_tokens) {
		*truncated = 0;
		return strdup(content);
	}
	/* Very small budgets still have a bounded, recognizable omission marker. */
	if (tokenizer_count(tokenizer, marker) > max_tokens)
		marker = "...";
	if (tokenizer_count(tokenizer, marker) > max_tokens)
		marker = ".";
	if (tokenizer_count(tokenizer, marker) > max_tokens)
		marker = "";
	budget = (size_t)((double)len * (double)max_tokens / (double)tokens);
	if (budget >= len)
		budget = len - 1;
	rc = morph_buf_init(&buf, 128);
	if (rc != 0)
		return NULL;
	for (;;) {
		size_t head = utf8_clamp_bytes(content, budget * 3 / 5);
		size_t tail = utf8_clamp_bytes(content, len - (budget - budget * 3 / 5));
		size_t next;

		morph_buf_reset(&buf);
		rc = morph_buf_append(&buf, content, head);
		if (rc == 0)
			rc = morph_buf_puts(&buf, marker);
		if (rc == 0)
			rc = morph_buf_puts(&buf, content + tail);
		if (rc != 0) {
			morph_buf_cleanup(&buf);
			return NULL;
		}
		tokens = tokenizer_count(tokenizer, morph_buf_cstr(&buf));
		if (tokens <= max_tokens)
			break;
		next = (size_t)((double)budget * (double)max_tokens / (double)tokens);
		budget = next < budget ? next : budget - 1;
	}
	*truncated = 1;
	return morph_buf_detach(&buf);
}

/* Only payload fields are expendable; execution control fields stay intact. */
static cJSON *history_largest_payload(cJSON *node, size_t *size)
{
	cJSON *best = NULL;
	cJSON *child;

	cJSON_ArrayForEach(child, node)
	{
		const char *key = child->string;
		if (key &&
		    (!strcmp(key, "ok") || !strcmp(key, "error") || !strcmp(key, "exit_code") ||
		     !strcmp(key, "status") || !strcmp(key, "session_id") ||
		     !strcmp(key, "artifacts") || !strcmp(key, "ui")))
			continue;
		if (cJSON_IsString(child) && strlen(child->valuestring) > *size) {
			*size = strlen(child->valuestring);
			best = child;
		} else if (cJSON_IsArray(child) && cJSON_GetArraySize(child) > 0) {
			char *json = cJSON_PrintUnformatted(child);
			if (json && strlen(json) > *size) {
				*size = strlen(json);
				best = child;
			}
			free(json);
		} else if (cJSON_IsObject(child)) {
			cJSON *nested = history_largest_payload(child, size);
			if (nested)
				best = nested;
		}
	}
	return best;
}

static char *history_prepare_json(struct tokenizer *tokenizer, cJSON *root, int budget,
				  int *truncated)
{
	cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
	cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
	cJSON *child;
	cJSON *next;

	if (!(cJSON_IsBool(ok) && cJSON_IsObject(data))) {
		char *json = cJSON_PrintUnformatted(root);
		cJSON *wrapper;
		char *result;

		if (!json || tokenizer_count(tokenizer, json) <= budget)
			return json;
		wrapper = cJSON_CreateObject();
		if (!wrapper) {
			free(json);
			return NULL;
		}
		if (!cJSON_AddBoolToObject(wrapper, "history_truncated", 1) ||
		    !cJSON_AddStringToObject(wrapper, "preview", json)) {
			free(json);
			cJSON_Delete(wrapper);
			return NULL;
		}
		free(json);
		for (int preview_budget = budget; preview_budget > 0; preview_budget /= 2) {
			int ignored;
			cJSON *preview = cJSON_GetObjectItemCaseSensitive(wrapper, "preview");
			char *snippet = history_truncate_result(tokenizer, preview->valuestring,
								preview_budget, &ignored);
			if (!snippet)
				break;
			if (!cJSON_SetValuestring(preview, snippet)) {
				free(snippet);
				break;
			}
			free(snippet);
			result = cJSON_PrintUnformatted(wrapper);
			if (result && tokenizer_count(tokenizer, result) <= budget) {
				*truncated = 1;
				cJSON_Delete(wrapper);
				return result;
			}
			free(result);
		}
		result = NULL;
		cJSON_Delete(wrapper);
		return result;
	}
	if (cJSON_IsBool(ok) && cJSON_IsObject(data)) {
		for (child = root->child; child; child = next) {
			cJSON *original;

			next = child->next;
			if (child == data || !child->string)
				continue;
			original = cJSON_GetObjectItemCaseSensitive(data, child->string);
			if (original && cJSON_Compare(original, child, 1))
				cJSON_DeleteItemFromObjectCaseSensitive(root, child->string);
		}
	}
	for (;;) {
		char *json = cJSON_PrintUnformatted(root);
		cJSON *largest;
		size_t size = 0;
		char *snippet;
		int ignored;
		int total_tokens;

		if (!json)
			return NULL;
		if (*truncated && cJSON_IsObject(root) &&
		    !cJSON_GetObjectItemCaseSensitive(root, "history_truncated")) {
			free(json);
			if (!cJSON_AddBoolToObject(root, "history_truncated", 1))
				return NULL;
			continue;
		}
		total_tokens = tokenizer_count(tokenizer, json);
		if (total_tokens <= budget)
			return json;
		free(json);
		largest = history_largest_payload(root, &size);
		if (!largest || size < 16)
			return NULL;
		if (cJSON_IsArray(largest)) {
			cJSON_DeleteItemFromArray(largest, cJSON_GetArraySize(largest) - 1);
			*truncated = 1;
			continue;
		}
		int text_tokens = tokenizer_count(tokenizer, largest->valuestring);
		int text_budget = text_tokens - (total_tokens - budget) - 16;
		snippet = history_truncate_result(tokenizer, largest->valuestring,
						  text_budget > 0 ? text_budget : 1, &ignored);
		if (!snippet)
			return NULL;
		if (!cJSON_SetValuestring(largest, snippet)) {
			free(snippet);
			return NULL;
		}
		free(snippet);
		*truncated = 1;
	}
}

int agent_history_prepare_tool_content(struct react_context *ctx,
	const char *content, char **prepared, int *truncated)
{
	char *limited;
	char *safe;
	int was_truncated = 0;

	if (!ctx || !prepared)
		MORPH_RETURN(-EINVAL);
	/* Repair malformed tool bytes before redaction or UTF-8 boundary selection. */
	limited = strdup(content ? content : "");
	if (!limited)
		MORPH_RETURN(-ENOMEM);
	utf8_sanitize_inplace(limited);
	/* Redaction may expand short secrets into longer replacement strings. */
	safe = history_redact_all(ctx, limited);
	free(limited);
	if (!safe)
		MORPH_RETURN(-ENOMEM);
	{
		cJSON *root = cJSON_ParseWithOpts(safe, NULL, 1);
		int budget = ctx->history_tool_result_tokens > 0
				 ? ctx->history_tool_result_tokens
				 : HISTORY_DEFAULT_TOOL_RESULT_MAX_TOKENS;

		if (root) {
			limited =
			    history_prepare_json(ctx->tokenizer, root, budget, &was_truncated);
			cJSON_Delete(root);
			if (!limited) {
				free(safe);
				MORPH_RETURN(-E2BIG);
			}
		} else {
			limited =
			    history_truncate_result(ctx->tokenizer, safe, budget, &was_truncated);
		}
	}
	free(safe);
	if (!limited)
		MORPH_RETURN(-ENOMEM);
	log_dbg(ctx ? ctx->logger : NULL, "history tool result: %zu -> %zu bytes, truncated=%d",
		content ? strlen(content) : 0, strlen(limited), was_truncated);
	*prepared = limited;
	if (truncated)
		*truncated = was_truncated;
	return 0;
}

int agent_history_record_tool_result_ex(struct react_context *ctx,
	const char *tool_call_id, const char *provider_call_id,
	const char *tool_name, const char *content, int error_code,
	const struct tool_artifact_list *artifacts, const cJSON *tool_meta)
{
	struct model_history_insert item = {0};
	char key[256];
	cJSON *metadata;
	char *payload = NULL;
	char *stored;
	int truncated = 0;
	int rc;

	if (!ctx)
		MORPH_RETURN(-EINVAL);
	rc = agent_history_prepare_tool_content(ctx, content, &stored,
		&truncated);
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
	snprintf(key, sizeof(key), "result:%s",
		tool_call_id ? tool_call_id : "unknown");
	item.idempotency_key = key;
	metadata = cJSON_CreateObject();
	if (!metadata) {
		free(stored);
		MORPH_RETURN(-ENOMEM);
	}
	cJSON_AddStringToObject(metadata, "status",
		error_code == -ETIMEDOUT ? "timed_out" :
		error_code == -ECANCELED ? "cancelled" :
		error_code < 0 ? "failed" : "completed");
	cJSON_AddNumberToObject(metadata, "error_code", error_code);
	cJSON_AddStringToObject(metadata, "tool_call_id",
		tool_call_id ? tool_call_id : "");
	cJSON_AddStringToObject(metadata, "provider_call_id",
		provider_call_id ? provider_call_id : "");
	cJSON_AddStringToObject(metadata, "tool_name",
		tool_name ? tool_name : "");
	cJSON_AddStringToObject(metadata, "content", stored);
	{
		char *original = strdup(content ? content : "");
		char *redacted;

		if (!original) {
			cJSON_Delete(metadata);
			free(stored);
			MORPH_RETURN(-ENOMEM);
		}
		utf8_sanitize_inplace(original);
		redacted = history_redact_all(ctx, original);
		free(original);
		if (!redacted || !cJSON_AddStringToObject(metadata, "original_content", redacted)) {
			free(redacted);
			cJSON_Delete(metadata);
			free(stored);
			MORPH_RETURN(-ENOMEM);
		}
		free(redacted);
	}
	if (artifacts && artifacts->count > 0) {
		cJSON *artifact_json = tool_artifact_list_to_json(artifacts);

		if (artifact_json)
			cJSON_AddItemToObject(metadata, "artifacts", artifact_json);
	}
	if (tool_meta)
		cJSON_AddItemToObject(metadata, "tool_meta",
			cJSON_Duplicate(tool_meta, 1));
	{
		cJSON *storage = cJSON_CreateObject();

		if (storage) {
			cJSON_AddBoolToObject(storage, "truncated", truncated);
			cJSON_AddNumberToObject(storage, "original_bytes",
				content ? (double)strlen(content) : 0.0);
			cJSON_AddNumberToObject(storage, "stored_bytes",
				(double)strlen(stored));
			cJSON_AddItemToObject(metadata, "storage", storage);
		}
	}
	payload = cJSON_PrintUnformatted(metadata);
	cJSON_Delete(metadata);
	if (!payload) {
		free(stored);
		MORPH_RETURN(-ENOMEM);
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

int agent_history_record_tool_result(struct react_context *ctx,
				     const char *tool_call_id,
				     const char *provider_call_id,
				     const char *tool_name,
				     const char *content,
				     int error_code)
{
	return agent_history_record_tool_result_ex(ctx, tool_call_id,
		provider_call_id, tool_name, content, error_code, NULL, NULL);
}

int agent_history_record_receipt(struct react_context *ctx,
	const char *name, const char *phase, const char *message,
	const char *task, int count, int error_code)
{
	struct model_history_insert item = {0};
	morph_buf_t text;
	cJSON *payload;
	char *json;
	char key[320];
	int rc;

	if (!ctx)
		MORPH_RETURN(-EINVAL);
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
		MORPH_RETURN(-ENOMEM);
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
		MORPH_RETURN(-ENOMEM);
	}
	snprintf(key, sizeof(key), "receipt:%s:%s:%s:%d",
		ctx->turn_id[0] ? ctx->turn_id : "unknown",
		name ? name : "", phase ? phase : "", count);
	item.session_id = ctx->history_session_id;
	item.turn_id = ctx->turn_id;
	item.kind = "background_receipt";
	item.role = "system";
	item.content = morph_buf_cstr(&text);
	item.payload_json = json;
	item.idempotency_key = key;
	item.token_count = tokenizer_count(ctx->tokenizer, item.content);
	item.active = 1;
	rc = history_store(ctx, &item);
	free(json);
	morph_buf_cleanup(&text);
	return rc;
}

static const char *history_compaction_text(const struct model_history_item *item,
	char **rendered)
{
	*rendered = NULL;
	if (!agent_history_item_replayable(item))
		return NULL;
	if (strcmp(item->kind, "user_message") == 0) {
		*rendered = history_user_content(item);
		return *rendered;
	}
	return strcmp(item->kind, "assistant_tool_calls") == 0 ?
		item->payload_json : item->content;
}

/* Steering can interleave a call and its output; remove both from candidates. */
static int history_result_in_omitted_group(const struct model_history_item *items,
					   const struct model_history_item *result, int64_t cutoff)
{
	for (const struct model_history_item *item = items; item; item = item->next) {
		if (item->sequence_no > cutoff || strcmp(item->kind, "assistant_tool_calls"))
			continue;
		cJSON *payload = item->payload_json ? cJSON_Parse(item->payload_json) : NULL;
		cJSON *call;
		int found = 0;
		cJSON_ArrayForEach(call, cJSON_GetObjectItem(payload, "calls"))
		{
			const char *local =
			    cJSON_GetStringValue(cJSON_GetObjectItem(call, "tool_call_id"));
			const char *provider =
			    cJSON_GetStringValue(cJSON_GetObjectItem(call, "provider_call_id"));
			if ((local && result->tool_call_id &&
			     !strcmp(local, result->tool_call_id)) ||
			    (provider && result->provider_call_id &&
			     !strcmp(provider, result->provider_call_id))) {
				found = 1;
				break;
			}
		}
		cJSON_Delete(payload);
		if (found)
			return 1;
	}
	return 0;
}

static int history_compaction_input(const struct model_history_item *items, morph_buf_t *buf,
				    int64_t omitted_through)
{
	const struct model_history_item *item = items;
	if (omitted_through &&
	    morph_buf_puts(buf, "Earlier tool groups were omitted to fit the summary request; do "
				"not infer their results.\n") != 0)
		MORPH_RETURN(-ENOMEM);

	while (item) {
		if ((!strcmp(item->kind, "assistant_tool_calls") &&
		     item->sequence_no <= omitted_through) ||
		    (!strcmp(item->kind, "tool_result") && omitted_through &&
		     history_result_in_omitted_group(items, item, omitted_through))) {
			item = item->next;
			continue;
		}
		const char *label = item->role[0] ? item->role : item->kind;
		char *rendered = NULL;
		const char *text = history_compaction_text(item, &rendered);
		int rc = 0;

		if (strcmp(item->kind, "user_message") == 0 && !rendered)
			MORPH_RETURN(-ENOMEM);
		if (text && text[0] &&
		    morph_buf_printf(buf, "[%s/%s]\n%s\n\n", label,
			item->kind, text) != 0)
			rc = -ENOMEM;
		free(rendered);
		if (rc != 0)
			MORPH_RETURN(-ENOMEM);
		item = item->next;
	}
	return 0;
}

int agent_history_reload(struct react_context *ctx)
{
	struct model_history_item *items;
	int count = 0;

	if (!ctx || !ctx->history_enabled || !ctx->history_db ||
	    ctx->history_session_id <= 0)
		MORPH_RETURN(-EINVAL);
	items = model_history_list(ctx->history_db, ctx->history_session_id, 1,
		&count);
	if (!items && count != 0)
		MORPH_RETURN(MORPH_ERR_DB);
	for (struct model_history_item *item = items; item; item = item->next) {
		cJSON *payload;
		const cJSON *original;
		char *prepared;
		int truncated;
		int rc;

		if (strcmp(item->kind, "tool_result") != 0)
			continue;
		payload = item->payload_json ? cJSON_Parse(item->payload_json) : NULL;
		original = cJSON_GetObjectItemCaseSensitive(payload, "original_content");
		rc = agent_history_prepare_tool_content(
		    ctx, cJSON_IsString(original) ? original->valuestring : item->content,
		    &prepared, &truncated);
		cJSON_Delete(payload);
		if (rc != 0) {
			model_history_free_list(items);
			return rc;
		}
		free(item->content);
		item->content = prepared;
		item->truncated = truncated;
		item->token_count = tokenizer_count(ctx->tokenizer, prepared);
	}
	model_history_free_list(ctx->history_items);
	ctx->history_items = items;
	return 0;
}

static int history_validate_replay(struct morph_log *logger, const struct model_history_item *items)
{
	struct arena *arena = arena_create(0);
	morph_array_t messages = {0};
	int rc;

	if (!arena)
		MORPH_RETURN(-ENOMEM);
	rc = morph_array_init(&messages, 64, sizeof(struct chat_message));
	if (rc == 0)
		rc = agent_history_build_chat_messages(logger, items, &messages, arena);
	if (rc == 0) {
		struct agent_history_diagnostic diagnostic;

		rc = agent_history_diagnose(items, NULL, &diagnostic);
		if (rc == 0 && (diagnostic.invalid_payloads ||
		    diagnostic.dangling_calls || diagnostic.orphan_results))
			rc = MORPH_ERR_PROTOCOL;
	}
	morph_array_cleanup(&messages);
	arena_destroy(arena);
	return rc;
}

/* Retained rows may have been counted by an older tokenizer or budget policy. */
static int history_refresh_retention_tokens(struct react_context *ctx)
{
	const char *sql = "SELECT a.id,a.kind,a.content,a.payload_json "
		"FROM model_history_items a WHERE a.session_id=? AND (a.active=1 OR "
		"(a.kind='assistant_message' AND EXISTS (SELECT 1 "
		"FROM model_history_items u WHERE u.session_id=a.session_id "
		"AND u.turn_id=a.turn_id AND u.active=1 AND u.kind='user_message')))";
	const char *update_sql = "UPDATE model_history_items SET token_count=? WHERE id=?";
	sqlite3_stmt *select = NULL;
	sqlite3_stmt *update = NULL;
	int error = 0;
	int rc = sqlite3_prepare_v2(ctx->history_db->handle, sql, -1, &select, NULL);

	if (rc == SQLITE_OK)
		rc = sqlite3_prepare_v2(ctx->history_db->handle, update_sql, -1,
			&update, NULL);
	if (rc != SQLITE_OK) {
		sqlite3_finalize(select);
		sqlite3_finalize(update);
		MORPH_RETURN(MORPH_ERR_DB);
	}
	sqlite3_bind_int64(select, 1, ctx->history_session_id);
	while ((rc = sqlite3_step(select)) == SQLITE_ROW) {
		struct model_history_item item = {0};

		strncpy(item.kind, (const char *)sqlite3_column_text(select, 1),
			sizeof(item.kind) - 1);
		item.content = (char *)sqlite3_column_text(select, 2);
		item.payload_json = (char *)sqlite3_column_text(select, 3);
		const char *text = strcmp(item.kind, "assistant_tool_calls") == 0 ?
			item.payload_json : item.content;
		int tokens = history_item_tokens(&item, ctx->tokenizer, text);

		if (tokens < 0) {
			MORPH_SET_ERR(error, tokens);
			break;
		}

		sqlite3_bind_int(update, 1, tokens);
		sqlite3_bind_int64(update, 2, sqlite3_column_int64(select, 0));
		if (sqlite3_step(update) != SQLITE_DONE) {
			MORPH_SET_ERR(error, MORPH_ERR_DB);
			break;
		}
		sqlite3_reset(update);
		sqlite3_clear_bindings(update);
	}
	sqlite3_finalize(select);
	sqlite3_finalize(update);
	if (error)
		return error;
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	return 0;
}

static int history_record_summary_sources(struct react_context *ctx, int64_t summary_id)
{
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(ctx->history_db->handle,
		"INSERT INTO history_summary_sources(summary_item_id,source_item_id) "
		"VALUES(?,?)", -1, &stmt, NULL);
	int error = 0;

	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	for (const struct model_history_item *item = ctx->history_items; item; item = item->next) {
		char *rendered = NULL;
		const char *text = history_compaction_text(item, &rendered);
		int has_text = text && text[0];

		if (strcmp(item->kind, "user_message") == 0 && !rendered)
			error = -ENOMEM;
		free(rendered);
		if (error)
			break;
		if (!has_text)
			continue;
		sqlite3_bind_int64(stmt, 1, summary_id);
		sqlite3_bind_int64(stmt, 2, item->id);
		rc = sqlite3_step(stmt);
		if (rc != SQLITE_DONE)
			break;
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
	}
	sqlite3_finalize(stmt);
	MORPH_RETURN(error ? error : rc == SQLITE_DONE || rc == SQLITE_OK ? 0 : MORPH_ERR_DB);
}

/* Keep the old window until the new database window can actually replay. */
static int history_compact_checked(struct react_context *ctx,
	const char *summary, int summary_tokens, int user_budget,
	int recent_budget, int input_tokens, int keep_rounds,
	const char *trigger_kind, int record_sources, int *active_count)
{
	struct model_history_item *candidate = NULL;
	int64_t summary_id = 0;
	int count = 0;
	int rc = db_exec(ctx->history_db, "SAVEPOINT history_replay;");

	if (rc != 0)
		return rc;
	rc = history_refresh_retention_tokens(ctx);
	if (rc == 0)
		rc = model_history_compact(ctx->history_db, ctx->history_session_id,
			ctx->turn_id, summary, summary_tokens, user_budget, recent_budget,
			input_tokens, keep_rounds, trigger_kind, &summary_id);
	if (rc == 0 && record_sources)
		rc = history_record_summary_sources(ctx, summary_id);
	if (rc == 0) {
		candidate = model_history_list(ctx->history_db,
			ctx->history_session_id, 1, &count);
		rc = count < 0 ? MORPH_ERR_DB
			       : history_validate_replay(ctx ? ctx->logger : NULL, candidate);
	}
	if (rc == 0)
		rc = db_exec(ctx->history_db, "RELEASE history_replay;");
	if (rc != 0) {
		(void)db_exec(ctx->history_db,
			"ROLLBACK TO history_replay; RELEASE history_replay;");
		model_history_free_list(candidate);
		return rc;
	}
	model_history_free_list(ctx->history_items);
	ctx->history_items = candidate;
	*active_count = count;
	return 0;
}

/* Byte limits are only an initial estimate; verify with the active tokenizer. */
static int history_limit_summary(struct react_context *ctx, char *summary,
				 int max_tokens)
{
	int tokens = tokenizer_count(ctx->tokenizer, summary);

	while (tokens > max_tokens && summary[0]) {
		size_t len = strlen(summary);
		size_t bytes = (size_t)((double)len *
			(double)max_tokens / (double)tokens);

		if (bytes >= len)
			bytes = len - 1;
		summary[utf8_clamp_bytes(summary, bytes)] = '\0';
		tokens = tokenizer_count(ctx->tokenizer, summary);
	}
	return tokens;
}

int agent_history_compact_with_trigger(struct react_context *ctx, int force,
	const char *trigger_kind_in)
{
	struct model_history_item *item;
	morph_buf_t input;
	char *summary = NULL;
	char *redacted = NULL;
	int max_summary_tokens;
	int user_budget;
	int recent_budget;
	int target_budget;
	int input_tokens = 0;
	int threshold;
	int count = 0;
	int record_sources;
	int64_t omitted_through = 0;
	int pruned_groups = 0;
	int rc;
	const char *trigger_kind = trigger_kind_in && trigger_kind_in[0] ?
		trigger_kind_in : (force ? "manual" : "threshold");

	if (!ctx || !ctx->history_enabled || !ctx->history_db ||
	    !ctx->compress.summarize || ctx->compress.max_context_tokens <= 0)
		return 0;
	for (item = ctx->history_items; item; item = item->next) {
		if (!agent_history_item_replayable(item))
			continue;
		if (item->token_count > 0)
			input_tokens = item->token_count > INT_MAX - input_tokens ?
				INT_MAX : input_tokens + item->token_count;
	}
	threshold = (int)((double)ctx->compress.max_context_tokens *
		ctx->compress.summarize_threshold_ratio);
	if (!force && input_tokens < threshold)
		return 0;
	if (!ctx->history_items)
		return 0;
	rc = morph_buf_init(&input, 8192);
	if (rc != 0)
		return rc;
	rc = history_compaction_input(ctx->history_items, &input, omitted_through);
	if (rc == 0) {
		for (int attempt = 0; attempt < 3; attempt++) {
			struct timespec started = {0};
			struct timespec finished = {0};
			int timed = clock_gettime(CLOCK_MONOTONIC, &started) == 0;

			log_info(ctx->logger,
				 "history summary request: session=%lld turn=%s trigger=%s "
				 "attempt=%d/3 input_bytes=%zu history_tokens=%d "
				 "summary_max_tokens=%d omitted_groups=%d",
				 (long long)ctx->history_session_id, ctx->turn_id,
				 trigger_kind, attempt + 1, input.len, input_tokens,
				 ctx->compress.compaction_summary_max_tokens, pruned_groups);
			rc = ctx->compress.summarize(morph_buf_cstr(&input),
						     ctx->compress.summarize_user_data, &summary);
			double elapsed_ms = timed &&
				clock_gettime(CLOCK_MONOTONIC, &finished) == 0 ?
				(double)(finished.tv_sec - started.tv_sec) * 1000.0 +
				(double)(finished.tv_nsec - started.tv_nsec) / 1000000.0 : 0.0;
			int empty = !summary || !summary[0];

			log_write(ctx->logger, rc == 0 && !empty ? LOG_INFO : LOG_WARN,
				  "history summary response: session=%lld turn=%s trigger=%s "
				  "attempt=%d/3 elapsed_ms=%.0f result=%s summary_bytes=%zu "
				  "error=%s",
				  (long long)ctx->history_session_id, ctx->turn_id,
				  trigger_kind, attempt + 1, elapsed_ms,
				  rc != 0 ? "request_failed" : empty ? "empty_summary" : "ok",
				  summary ? strlen(summary) : 0, morph_strerror(rc));
			if (rc == 0 && summary && summary[0])
				break;
			if (rc == 0)
				rc = MORPH_ERR_PROTOCOL;
			free(summary);
			summary = NULL;
			if (rc == MORPH_ERR_CONTEXT_LIMIT && pruned_groups < 32) {
				const struct model_history_item *group = ctx->history_items;
				while (group && (group->sequence_no <= omitted_through ||
						 strcmp(group->kind, "assistant_tool_calls")))
					group = group->next;
				if (!group)
					break;
				omitted_through = group->sequence_no;
				for (group = group->next;
				     group && strcmp(group->kind, "assistant_tool_calls") &&
				     strcmp(group->kind, "user_message");
				     group = group->next)
					omitted_through = group->sequence_no;
				pruned_groups++;
				morph_buf_reset(&input);
				rc = history_compaction_input(ctx->history_items, &input,
							      omitted_through);
				if (rc != 0)
					break;
				log_warn(ctx ? ctx->logger : NULL,
					 "summary context recovery: omitted %d old tool groups",
					 pruned_groups);
				attempt--;
				continue;
			}
			if (attempt == 2 || (rc != MORPH_ERR_NETWORK && rc != MORPH_ERR_PROTOCOL &&
					     rc != -ETIMEDOUT))
				break;
			log_warn(ctx->logger,
				 "history summary retry scheduled: session=%lld turn=%s "
				 "next_attempt=%d/3 delay_ms=%u error=%s",
				 (long long)ctx->history_session_id, ctx->turn_id,
				 attempt + 2, (unsigned int)(attempt + 1) * 1000u,
				 morph_strerror(rc));
			rc = http_wait_cancelable(
				&(struct http_control){.cancel_token = &ctx->cancel_token,
						       .cancel_flag = &ctx->cancelled},
				(unsigned int)(attempt + 1) * 1000u);
			if (rc != 0)
				break;
		}
	}
	morph_buf_cleanup(&input);
	record_sources = rc == 0 && summary && summary[0] && !pruned_groups;
	if (rc != 0 || !summary || !summary[0]) {
		int failure = rc != 0 ? rc : MORPH_ERR_PROTOCOL;

		free(summary);
		(void)model_history_compaction_attempt_add(ctx->history_db,
			ctx->history_session_id, ctx->turn_id, trigger_kind,
			"failed", input_tokens, 0, failure,
			morph_strerror(failure));
		MORPH_RETURN(failure);
	}
	if (pruned_groups) {
		morph_buf_t marked;
		if (morph_buf_init(&marked, 256) != 0) {
			free(summary);
			MORPH_RETURN(-ENOMEM);
		}
		rc = morph_buf_printf(
		    &marked,
		    "%d older tool groups were omitted; their outcomes are unavailable.\n%s",
		    pruned_groups, summary);
		free(summary);
		if (rc != 0) {
			morph_buf_cleanup(&marked);
			return rc;
		}
		summary = morph_buf_detach(&marked);
	}
	redacted = history_redact_all(ctx, summary);
	free(summary);
	summary = redacted;
	if (!summary)
		MORPH_RETURN(-ENOMEM);
	max_summary_tokens = ctx->compress.compaction_summary_max_tokens;
	max_summary_tokens = max_summary_tokens > 0 ?
		history_limit_summary(ctx, summary, max_summary_tokens) :
		tokenizer_count(ctx->tokenizer, summary);
	if (!summary[0]) {
		free(summary);
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	}
	user_budget = (int)((double)ctx->compress.max_context_tokens *
		ctx->compress.compress_target_ratio) - max_summary_tokens;
	if (user_budget < 0)
		user_budget = 0;
	if (user_budget > ctx->compress.compaction_user_message_tokens)
		user_budget = ctx->compress.compaction_user_message_tokens;
	target_budget = (int)((double)ctx->compress.max_context_tokens *
		ctx->compress.compress_target_ratio);
	recent_budget = target_budget - max_summary_tokens - user_budget;
	if (recent_budget < 0)
		recent_budget = 0;
	rc = history_compact_checked(ctx, summary, max_summary_tokens,
		user_budget, recent_budget, input_tokens,
		ctx->compress.max_history_rounds,
		trigger_kind, record_sources, &count);
	free(summary);
	if (rc != 0) {
		(void)model_history_compaction_attempt_add(ctx->history_db,
			ctx->history_session_id, ctx->turn_id, trigger_kind,
			"failed", input_tokens, 0, rc, morph_strerror(rc));
		return rc;
	}
	(void)model_history_compaction_attempt_add(ctx->history_db,
		ctx->history_session_id, ctx->turn_id, trigger_kind,
		"completed", input_tokens, max_summary_tokens, 0, NULL);
	log_info(ctx ? ctx->logger : NULL,
		 "model history compaction %s: session=%lld turn=%s trigger=%s "
		 "history_tokens=%d summary_tokens=%d active_items=%d "
		 "user_budget=%d recent_budget=%d omitted_groups=%d",
		 strncmp(trigger_kind, "in_turn_", 8) == 0 ? "prepared" : "committed",
		 (long long)ctx->history_session_id, ctx->turn_id, trigger_kind,
		 input_tokens, max_summary_tokens, count, user_budget, recent_budget,
		 pruned_groups);
	if (ctx->compress.compaction_warning_count > 0) {
		int compactions = model_history_compaction_count(
			ctx->history_db, ctx->history_session_id);

		if (compactions >= ctx->compress.compaction_warning_count) {
			cJSON *data = cJSON_CreateObject();

			log_warn(ctx ? ctx->logger : NULL,
				 "model history has been compacted %d times; "
				 "consider starting a new session",
				 compactions);
			if (data) {
				cJSON_AddNumberToObject(data, "count", compactions);
				(void)morph_event_emit_simple(ctx->event_cb,
					ctx->event_user_data, MORPH_EVENT_BACKGROUND,
					"history.compaction.warning", "warning",
					"Consider starting a new session; repeated "
					"compaction loses detail.", data);
				cJSON_Delete(data);
			}
		}
	}
	return 1;
}

int agent_history_compact(struct react_context *ctx, int force)
{
	return agent_history_compact_with_trigger(ctx, force, NULL);
}

int agent_history_maybe_compact(struct react_context *ctx)
{
	return agent_history_compact(ctx, 0);
}

static int history_result_exists(const struct model_history_item *items,
				 const char *local_id,
				 const char *provider_id)
{
	for (const struct model_history_item *item = items; item;
	     item = item->next) {
		if (!item->active || strcmp(item->kind, "tool_result") != 0)
			continue;
		if (local_id && local_id[0] && item->tool_call_id &&
		    item->tool_call_id[0]) {
			if (strcmp(local_id, item->tool_call_id) == 0)
				return 1;
			continue;
		}
		if (provider_id && item->provider_call_id &&
		    strcmp(provider_id, item->provider_call_id) == 0)
			return 1;
	}
	return 0;
}

int agent_history_repair_interrupted(struct db *db, int64_t session_id)
{
	const char *interrupted =
		"tool error: previous execution was interrupted before a result "
		"was recorded";
	struct model_history_item *items;
	struct model_history_item *item;
	int count = 0;
	int rc = 0;

	if (!db || session_id <= 0)
		MORPH_RETURN(-EINVAL);
	items = model_history_list(db, session_id, 1, &count);
	for (item = items; item && rc == 0; item = item->next) {
		cJSON *root;
		cJSON *calls;

		if (strcmp(item->kind, "assistant_tool_calls") != 0 ||
		    !item->payload_json)
			continue;
		root = cJSON_ParseWithOpts(item->payload_json, NULL, 1);
		calls = root ? cJSON_GetObjectItem(root, "calls") : NULL;
		if (!cJSON_IsArray(calls)) {
			cJSON_Delete(root);
			continue;
		}
		for (int i = 0; i < cJSON_GetArraySize(calls); i++) {
			cJSON *call = cJSON_GetArrayItem(calls, i);
			cJSON *local = cJSON_GetObjectItem(call, "tool_call_id");
			cJSON *provider = cJSON_GetObjectItem(call,
				"provider_call_id");
			cJSON *name = cJSON_GetObjectItem(call, "name");
			struct model_history_insert repair = {0};
			const char *input;
			enum tool_input_kind input_kind;

			if (!cJSON_IsString(local) || !cJSON_IsString(provider) ||
			    !cJSON_IsString(name) ||
			    !history_tool_call_input(call, &input_kind, &input) ||
			    history_result_exists(items, local->valuestring,
				provider->valuestring))
				continue;
			repair.session_id = session_id;
			repair.turn_id = item->turn_id;
			repair.kind = "tool_result";
			repair.role = "tool";
			repair.content = interrupted;
			repair.payload_json =
				"{\"status\":\"interrupted\"}";
			repair.tool_call_id = local->valuestring;
			repair.provider_call_id = provider->valuestring;
			repair.tool_name = name->valuestring;
			{
				char repair_key[256];

				snprintf(repair_key, sizeof(repair_key), "result:%s",
					 local->valuestring);
				repair.idempotency_key = repair_key;
				repair.token_count =
					tokenizer_estimate_tokens(interrupted);
				repair.active = 1;
				rc = model_history_add(db, &repair, NULL);
			}
			if (rc != 0)
				break;
		}
		cJSON_Delete(root);
	}
	model_history_free_list(items);
	return rc;
}

static int history_call_exists(const struct model_history_item *items,
			       const char *local_id,
			       const char *provider_id)
{
	for (const struct model_history_item *item = items; item;
	     item = item->next) {
		cJSON *root;
		cJSON *calls;
		int found = 0;

		if (!item->active || strcmp(item->kind, "assistant_tool_calls") != 0 ||
		    !item->payload_json)
			continue;
		root = cJSON_ParseWithOpts(item->payload_json, NULL, 1);
		calls = root ? cJSON_GetObjectItem(root, "calls") : NULL;
		if (cJSON_IsArray(calls)) {
			for (int i = 0; i < cJSON_GetArraySize(calls); i++) {
				cJSON *call = cJSON_GetArrayItem(calls, i);
				cJSON *local = cJSON_GetObjectItem(call,
					"tool_call_id");
				cJSON *provider = cJSON_GetObjectItem(call,
					"provider_call_id");
				cJSON *name = cJSON_GetObjectItem(call, "name");
				const char *input;
				enum tool_input_kind input_kind;

				if (!cJSON_IsString(local) ||
				    !cJSON_IsString(provider) ||
				    !cJSON_IsString(name) ||
				    !history_tool_call_input(call, &input_kind,
					&input))
					continue;
				if (local_id && local_id[0] &&
				    local->valuestring[0]) {
					if (strcmp(local_id,
						   local->valuestring) == 0)
						found = 1;
				} else if (provider_id &&
					   strcmp(provider_id,
						  provider->valuestring) == 0) {
					found = 1;
				}
				if (found)
					break;
			}
		}
		cJSON_Delete(root);
		if (found)
			return 1;
	}
	return 0;
}

int agent_history_diagnose(const struct model_history_item *items,
	struct tokenizer *tokenizer, struct agent_history_diagnostic *diagnostic)
{
	const struct model_history_item *item;

	if (!diagnostic)
		MORPH_RETURN(-EINVAL);
	memset(diagnostic, 0, sizeof(*diagnostic));
	for (item = items; item; item = item->next) {
		const char *token_text = item->content;

		if (!item->active)
			continue;
		diagnostic->active_items++;
		if (strcmp(item->kind, "assistant_tool_calls") == 0) {
			cJSON *root = item->payload_json ?
				cJSON_ParseWithOpts(item->payload_json, NULL, 1) : NULL;
			cJSON *calls = root ?
				cJSON_GetObjectItem(root, "calls") : NULL;

			token_text = item->payload_json;
			if (!cJSON_IsArray(calls)) {
				diagnostic->invalid_payloads++;
			} else {
				for (int i = 0; i < cJSON_GetArraySize(calls); i++) {
					cJSON *call = cJSON_GetArrayItem(calls, i);
					cJSON *local = cJSON_GetObjectItem(call,
						"tool_call_id");
					cJSON *provider = cJSON_GetObjectItem(call,
						"provider_call_id");
					cJSON *name = cJSON_GetObjectItem(call,
						"name");
					const char *input;
					enum tool_input_kind input_kind;

					if (!cJSON_IsString(local) ||
					    !cJSON_IsString(provider) ||
					    !cJSON_IsString(name) ||
					    !history_tool_call_input(call, &input_kind,
						&input)) {
						diagnostic->invalid_payloads++;
						continue;
					}
					if (!history_result_exists(items,
						local->valuestring,
						provider->valuestring))
						diagnostic->dangling_calls++;
				}
			}
			cJSON_Delete(root);
		} else if (strcmp(item->kind, "tool_result") == 0 &&
			   !history_call_exists(items, item->tool_call_id,
				item->provider_call_id)) {
			diagnostic->orphan_results++;
		}
		int tokens = history_item_tokens(item, tokenizer, token_text);

		if (tokens < 0)
			return tokens;
		if (tokens != item->token_count)
			diagnostic->token_mismatches++;
	}
	return 0;
}

static int history_update_item(struct db *db, int64_t id, int active,
			       int token_count)
{
	const char *sql =
		"UPDATE model_history_items SET active=?,token_count=? WHERE id=?";
	sqlite3_stmt *stmt;
	int rc;

	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int(stmt, 1, active);
	sqlite3_bind_int(stmt, 2, token_count);
	sqlite3_bind_int64(stmt, 3, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	return 0;
}

int agent_history_repair(struct db *db, int64_t session_id,
	struct tokenizer *tokenizer,
	struct agent_history_diagnostic *before, int *changed)
{
	struct model_history_item *items;
	struct agent_history_diagnostic local;
	int count = 0;
	int changes = 0;
	int rc;

	if (!db || session_id <= 0)
		MORPH_RETURN(-EINVAL);
	rc = db_exec(db, "BEGIN IMMEDIATE;");
	if (rc != 0)
		return rc;
	items = model_history_list(db, session_id, 1, &count);
	if (!items && count != 0) {
		(void)db_exec(db, "ROLLBACK;");
		MORPH_RETURN(MORPH_ERR_DB);
	}
	rc = agent_history_diagnose(items, tokenizer,
		before ? before : &local);
	model_history_free_list(items);
	if (rc != 0) {
		(void)db_exec(db, "ROLLBACK;");
		return rc;
	}
	changes = before ? before->dangling_calls : local.dangling_calls;
	/* Legacy answers prove a response was delivered, not task success. */
	rc = history_store_turn_outcomes(db, session_id, NULL, "answered", 0,
		tokenizer, &changes);
	if (rc != 0) {
		(void)db_exec(db, "ROLLBACK;");
		return rc;
	}
	rc = agent_history_repair_interrupted(db, session_id);
	if (rc != 0) {
		(void)db_exec(db, "ROLLBACK;");
		return rc;
	}
	items = model_history_list(db, session_id, 1, &count);
	if (!items && count != 0) {
		(void)db_exec(db, "ROLLBACK;");
		MORPH_RETURN(MORPH_ERR_DB);
	}
	for (struct model_history_item *item = items; item; item = item->next) {
		const char *token_text = item->content;
		int active = strcmp(item->kind, "background_receipt") == 0 ?
			agent_history_item_replayable(item) : 1;
		int tokens;

		if (strcmp(item->kind, "assistant_tool_calls") == 0) {
			cJSON *root = item->payload_json ?
				cJSON_ParseWithOpts(item->payload_json, NULL, 1) : NULL;
			cJSON *calls = root ?
				cJSON_GetObjectItem(root, "calls") : NULL;

			token_text = item->payload_json;
			if (!cJSON_IsArray(calls) ||
			    cJSON_GetArraySize(calls) <= 0) {
				active = 0;
			} else {
				for (int i = 0; i < cJSON_GetArraySize(calls); i++) {
					cJSON *call = cJSON_GetArrayItem(calls, i);
					const char *input;
					enum tool_input_kind input_kind;
					int input_valid = history_tool_call_input(
						call, &input_kind, &input);

					if (!cJSON_IsString(cJSON_GetObjectItem(call,
						"tool_call_id")) ||
					    !cJSON_IsString(cJSON_GetObjectItem(call,
						"provider_call_id")) ||
					    !cJSON_IsString(cJSON_GetObjectItem(call,
						"name")) ||
					    !input_valid) {
						active = 0;
						break;
					}
				}
			}
			cJSON_Delete(root);
		} else if (strcmp(item->kind, "tool_result") == 0 &&
			   !history_call_exists(items, item->tool_call_id,
				item->provider_call_id)) {
			active = 0;
			log_warn(db ? db->logger : NULL,
				 "deactivating orphan history tool result %lld",
				 (long long)item->id);
		}
		tokens = history_item_tokens(item, tokenizer, token_text);
		if (tokens < 0) {
			model_history_free_list(items);
			(void)db_exec(db, "ROLLBACK;");
			return tokens;
		}
		if (active != item->active || tokens != item->token_count) {
			rc = history_update_item(db, item->id, active, tokens);
			if (rc != 0) {
				model_history_free_list(items);
				(void)db_exec(db, "ROLLBACK;");
				return rc;
			}
			changes++;
		}
	}
	model_history_free_list(items);
	items = model_history_list(db, session_id, 1, &count);
	if (!items && count != 0) {
		(void)db_exec(db, "ROLLBACK;");
		MORPH_RETURN(MORPH_ERR_DB);
	}
	for (struct model_history_item *item = items; item; item = item->next) {
		if (strcmp(item->kind, "tool_result") != 0 ||
		    history_call_exists(items, item->tool_call_id,
			item->provider_call_id))
			continue;
		log_warn(db ? db->logger : NULL, "deactivating orphan history tool result %lld",
			 (long long)item->id);
		rc = history_update_item(db, item->id, 0, item->token_count);
		if (rc != 0) {
			model_history_free_list(items);
			(void)db_exec(db, "ROLLBACK;");
			return rc;
		}
		changes++;
	}
	model_history_free_list(items);
	rc = db_exec(db, "COMMIT;");
	if (rc != 0) {
		(void)db_exec(db, "ROLLBACK;");
		return rc;
	}
	if (changed)
		*changed = changes;
	return 0;
}
