#include "history.h"
#include "react.h"
#include "tokenizer.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/log.h"
#include "util/utf8.h"
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HISTORY_DEFAULT_TOOL_RESULT_MAX_TOKENS 8000
#define HISTORY_BYTES_PER_TOKEN 4

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

			if (base64 && (size_t)(base64 - cur) < 128) {
				rc = morph_buf_puts(&buf, "[binary data omitted]");
				cur = base64 + strlen(";base64,");
				while (*cur && !isspace((unsigned char)*cur) &&
				       *cur != '"' && *cur != '\'')
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

static int history_tool_arguments_valid(const cJSON *arguments)
{
	cJSON *parsed;
	int valid;

	if (!cJSON_IsString(arguments) || !arguments->valuestring)
		return 0;
	parsed = cJSON_Parse(arguments->valuestring);
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
	parsed = cJSON_Parse(raw);
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

static int history_add_tool_calls(const struct model_history_item *item,
				  morph_array_t *messages,
				  struct arena *arena)
{
	struct chat_message *message;
	cJSON *calls;
	cJSON *root;
	int count;

	root = item->payload_json ? cJSON_Parse(item->payload_json) : NULL;
	if (!root)
		MORPH_RETURN(MORPH_ERR_PARSE);
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

int agent_history_build_chat_messages(const struct model_history_item *items,
				      morph_array_t *messages,
				      struct arena *arena)
{
	const struct model_history_item *item = items;
	int rc;

	if (!messages || !arena)
		MORPH_RETURN(-EINVAL);
	while (item) {
		if (strcmp(item->kind, "assistant_tool_calls") == 0)
			rc = history_add_tool_calls(item, messages, arena);
		else if (strcmp(item->kind, "tool_result") == 0)
			rc = history_add_tool_result(item, messages, arena);
		else if (strcmp(item->kind, "compaction_summary") == 0)
			rc = history_add_text_message(messages, arena, "system",
				item->content);
		else if (strcmp(item->kind, "user_message") == 0)
			rc = history_add_text_message(messages, arena, "user",
				item->content);
		else if (strcmp(item->kind, "assistant_message") == 0)
			rc = history_add_text_message(messages, arena, "assistant",
				item->content);
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
	content = history_redact_all(ctx, insert->content);
	payload = history_redact_all(ctx, insert->payload_json);
	if ((insert->content && !content) || (insert->payload_json && !payload)) {
		free(content);
		free(payload);
		MORPH_RETURN(-ENOMEM);
	}
	safe = *insert;
	safe.content = content;
	safe.payload_json = payload;
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
	return history_store(ctx, &item);
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

static char *history_truncate_result(const char *content, int max_tokens,
				    int *truncated)
{
	morph_buf_t buf;
	size_t head_bytes;
	size_t max_bytes;
	size_t prefix_len;
	size_t tail_bytes;
	size_t len;
	size_t stored_bytes;
	char marker[160];
	int rc;

	if (!content)
		content = "";
	if (max_tokens <= 0)
		max_tokens = HISTORY_DEFAULT_TOOL_RESULT_MAX_TOKENS;
	max_bytes = (size_t)max_tokens * HISTORY_BYTES_PER_TOKEN;
	len = strlen(content);
	if (len <= max_bytes) {
		*truncated = 0;
		return strdup(content);
	}
	head_bytes = max_bytes * 3 / 5;
	tail_bytes = max_bytes - head_bytes;
	head_bytes = utf8_clamp_bytes(content, head_bytes);
	prefix_len = utf8_clamp_bytes(content, len - tail_bytes);
	stored_bytes = head_bytes + (len - prefix_len);
	for (int i = 0; i < 3; i++) {
		int marker_len = snprintf(marker, sizeof(marker),
			"\n…(tool output truncated: original_bytes=%zu, "
			"stored_bytes=%zu)…\n", len, stored_bytes);

		if (marker_len < 0 || (size_t)marker_len >= sizeof(marker))
			return NULL;
		stored_bytes = head_bytes + (len - prefix_len) +
			(size_t)marker_len;
	}
	rc = morph_buf_init(&buf, max_bytes + 128);
	if (rc != 0)
		return NULL;
	rc = morph_buf_append(&buf, content, head_bytes);
	if (rc == 0)
		rc = morph_buf_puts(&buf, marker);
	if (rc == 0)
		rc = morph_buf_puts(&buf, content + prefix_len);
	if (rc != 0) {
		morph_buf_cleanup(&buf);
		return NULL;
	}
	*truncated = 1;
	return morph_buf_detach(&buf);
}

int agent_history_prepare_tool_content(struct react_context *ctx,
	const char *content, char **prepared, int *truncated)
{
	char *limited;
	char *safe;
	int was_truncated = 0;

	if (!ctx || !prepared)
		MORPH_RETURN(-EINVAL);
	limited = history_truncate_result(content,
		ctx->history_tool_result_tokens, &was_truncated);
	if (!limited)
		MORPH_RETURN(-ENOMEM);
	safe = history_redact_all(ctx, limited);
	free(limited);
	if (!safe)
		MORPH_RETURN(-ENOMEM);
	*prepared = safe;
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

static int history_compaction_input(const struct model_history_item *items,
				    morph_buf_t *buf)
{
	const struct model_history_item *item = items;

	while (item) {
		const char *label = item->role[0] ? item->role : item->kind;
		const char *text = item->content;

		if (strcmp(item->kind, "assistant_tool_calls") == 0)
			text = item->payload_json;
		if (text && text[0] &&
		    morph_buf_printf(buf, "[%s/%s]\n%s\n\n", label,
			item->kind, text) != 0)
			MORPH_RETURN(-ENOMEM);
		item = item->next;
	}
	return 0;
}

int agent_history_compact(struct react_context *ctx, int force)
{
	struct model_history_item *item;
	morph_buf_t input;
	char *summary = NULL;
	char *limited = NULL;
	char *redacted = NULL;
	int max_summary_tokens;
	int user_budget;
	int input_tokens = 0;
	int threshold;
	int count = 0;
	int rc;
	const char *trigger_kind = force ? "manual" : "threshold";

	if (!ctx || !ctx->history_enabled || !ctx->history_db ||
	    !ctx->compress.summarize || ctx->compress.max_context_tokens <= 0)
		return 0;
	for (item = ctx->history_items; item; item = item->next)
		input_tokens += item->token_count;
	threshold = (int)((double)ctx->compress.max_context_tokens *
		ctx->compress.summarize_threshold_ratio);
	if (!force && input_tokens < threshold)
		return 0;
	if (!ctx->history_items)
		return 0;
	rc = morph_buf_init(&input, 8192);
	if (rc != 0)
		return rc;
	rc = history_compaction_input(ctx->history_items, &input);
	if (rc == 0)
		rc = ctx->compress.summarize(morph_buf_cstr(&input),
			ctx->compress.summarize_user_data, &summary);
	morph_buf_cleanup(&input);
	if (rc != 0 || !summary || !summary[0]) {
		int failure = rc != 0 ? rc : MORPH_ERR_PROTOCOL;

		free(summary);
		(void)model_history_compaction_attempt_add(ctx->history_db,
			ctx->history_session_id, ctx->turn_id, trigger_kind,
			"failed", input_tokens, 0, failure,
			morph_strerror(failure));
		if (force)
			return failure;
		if (input_tokens < ctx->compress.max_context_tokens) {
			log_warn("history compaction failed before hard limit: %s",
				 morph_strerror(failure));
			return 0;
		}
		summary = strdup(
			"[Earlier context could not be summarized. The most recent "
			"complete turns are preserved below; treat earlier details "
			"as unavailable and do not assume unfinished work completed.]");
		if (!summary)
			MORPH_RETURN(-ENOMEM);
		trigger_kind = "fallback";
	}
	max_summary_tokens = ctx->compress.compaction_summary_max_tokens;
	if (max_summary_tokens > 0 &&
	    tokenizer_count(ctx->tokenizer, summary) > max_summary_tokens) {
		limited = utf8_dup_clamped(summary,
			(size_t)max_summary_tokens * HISTORY_BYTES_PER_TOKEN);
		free(summary);
		summary = limited;
		if (!summary)
			MORPH_RETURN(-ENOMEM);
	}
	redacted = history_redact_all(ctx, summary);
	free(summary);
	summary = redacted;
	if (!summary)
		MORPH_RETURN(-ENOMEM);
	max_summary_tokens = tokenizer_count(ctx->tokenizer, summary);
	user_budget = (int)((double)ctx->compress.max_context_tokens *
		ctx->compress.compress_target_ratio) - max_summary_tokens;
	if (user_budget < 0)
		user_budget = 0;
	if (user_budget > ctx->compress.compaction_user_message_tokens)
		user_budget = ctx->compress.compaction_user_message_tokens;
	rc = model_history_compact(ctx->history_db, ctx->history_session_id,
		ctx->turn_id, summary, max_summary_tokens, user_budget,
		input_tokens,
		strcmp(trigger_kind, "fallback") == 0 ?
			ctx->compress.max_history_rounds : 0,
		trigger_kind, NULL);
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
	model_history_free_list(ctx->history_items);
	ctx->history_items = model_history_list(ctx->history_db,
		ctx->history_session_id, 1, &count);
	if (!ctx->history_items && count != 0)
		MORPH_RETURN(MORPH_ERR_DB);
	log_info("model history compacted: %d input tokens, %d active items",
		 input_tokens, count);
	if (ctx->compress.compaction_warning_count > 0) {
		int compactions = model_history_compaction_count(
			ctx->history_db, ctx->history_session_id);

		if (compactions >= ctx->compress.compaction_warning_count) {
			cJSON *data = cJSON_CreateObject();

			log_warn("model history has been compacted %d times; "
				 "consider starting a new session", compactions);
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
		if (strcmp(item->kind, "tool_result") != 0)
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
		root = cJSON_Parse(item->payload_json);
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

		if (strcmp(item->kind, "assistant_tool_calls") != 0 ||
		    !item->payload_json)
			continue;
		root = cJSON_Parse(item->payload_json);
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
				cJSON_Parse(item->payload_json) : NULL;
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
		if (tokenizer_count(tokenizer, token_text ? token_text : "") !=
		    item->token_count)
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
		int active = 1;
		int tokens;

		if (strcmp(item->kind, "assistant_tool_calls") == 0) {
			cJSON *root = item->payload_json ?
				cJSON_Parse(item->payload_json) : NULL;
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
			log_warn("deactivating orphan history tool result %lld",
				 (long long)item->id);
		}
		tokens = tokenizer_count(tokenizer, token_text ? token_text : "");
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
		log_warn("deactivating orphan history tool result %lld",
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
