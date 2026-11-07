#include "history_internal.h"

#include "prompt_context.h"
#include "react.h"
#include "tokenizer.h"
#include "util/cleanup.h"
#include "util/json.h"
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

static struct chat_message *history_push_message(morph_array_t *messages)
{
	struct chat_message *message = morph_array_push_borrow(messages);

	if (message)
		memset(message, 0, sizeof(*message));
	return message;
}

static int history_add_text_message(morph_array_t *messages, const char *role, const char *content)
{
	struct chat_message *message = history_push_message(messages);

	if (!message)
		return -ENOMEM;
	message->role = strdup(role ? role : "user");
	message->content = strdup(content ? content : "");
	if (!message->role || !message->content)
		return -ENOMEM;
	return 0;
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

int history_tool_input_borrow(const cJSON *call, enum tool_input_kind *kind, const char **input)
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

int agent_history_tool_arguments_copy(const char *arguments, char **normalized)
{
	const char *raw = arguments ? arguments : "{}";
	cJSON *parsed;
	cJSON *fallback;
	char *json MORPH_CLEANUP(morph_json_text_cleanup) = NULL;

	if (!normalized)
		return -EINVAL;
	if (*normalized)
		return -EALREADY;
	parsed = cJSON_ParseWithOpts(raw, NULL, 1);
	if (cJSON_IsObject(parsed)) {
		cJSON_Delete(parsed);
		*normalized = strdup(raw);
		if (!*normalized)
			return -ENOMEM;
		return 0;
	}
	cJSON_Delete(parsed);
	fallback = cJSON_CreateObject();
	if (!fallback)
		return -ENOMEM;
	if (!cJSON_AddStringToObject(fallback, "_morph_invalid_arguments", raw) ||
	    !cJSON_AddStringToObject(fallback, "_morph_recovery",
				     "Reissue this tool call with a valid JSON object.")) {
		cJSON_Delete(fallback);
		return -ENOMEM;
	}
	json = cJSON_PrintUnformatted(fallback);
	cJSON_Delete(fallback);
	if (!json)
		return -ENOMEM;
	*normalized = strdup(json);
	return *normalized ? 0 : -ENOMEM;
}

int agent_history_migrate_legacy_tool_call(struct tool_call *call)
{
	cJSON *arguments;
	char *normalized MORPH_CLEANUP(morph_json_text_cleanup) = NULL;
	char *owned MORPH_CLEANUP(morph_charp_cleanup) = NULL;

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
		return -ENOMEM;
	owned = strdup(normalized);
	if (!owned)
		return -ENOMEM;
	free(call->arguments);
	call->arguments = morph_charp_steal(&owned);
	return 0;
}

static int history_add_tool_calls(struct morph_log *logger, const struct model_history_item *item,
				  morph_array_t *messages)
{
	struct chat_message *message;
	cJSON *calls;
	cJSON *root;
	int count;

	root = item->payload_json ? cJSON_ParseWithOpts(item->payload_json, NULL, 1) : NULL;
	if (!root) {
		log_err(logger, "history replay: invalid tool payload at item %lld, sequence %lld",
			(long long)item->id, (long long)item->sequence_no);
		return MORPH_ERR_PARSE;
	}
	calls = cJSON_GetObjectItem(root, "calls");
	count = cJSON_IsArray(calls) ? cJSON_GetArraySize(calls) : 0;
	if (count <= 0) {
		cJSON_Delete(root);
		return MORPH_ERR_PROTOCOL;
	}
	message = history_push_message(messages);
	if (!message) {
		cJSON_Delete(root);
		return -ENOMEM;
	}
	message->role = strdup("assistant");
	message->content = item->content && item->content[0] ? strdup(item->content) : NULL;
	cJSON *reasoning = cJSON_GetObjectItem(root, "reasoning_content");
	if (cJSON_IsString(reasoning) && reasoning->valuestring)
		message->reasoning_content = strdup(reasoning->valuestring);
	message->tool_calls = calloc(1, (size_t)count * sizeof(*message->tool_calls));
	if (!message->role || !message->tool_calls ||
	    (cJSON_IsString(reasoning) && !message->reasoning_content)) {
		cJSON_Delete(root);
		return -ENOMEM;
	}
	memset(message->tool_calls, 0, (size_t)count * sizeof(*message->tool_calls));
	message->tool_call_count = count;
	for (int i = 0; i < count; i++) {
		cJSON *call = cJSON_GetArrayItem(calls, i);
		cJSON *provider_id = cJSON_GetObjectItem(call, "provider_call_id");
		cJSON *local_id = cJSON_GetObjectItem(call, "tool_call_id");
		cJSON *name = cJSON_GetObjectItem(call, "name");
		struct tool_call *tool_call = &message->tool_calls[i];
		const char *input;
		enum tool_input_kind input_kind;

		if (!cJSON_IsString(provider_id) || !cJSON_IsString(local_id) ||
		    !cJSON_IsString(name) ||
		    !history_tool_input_borrow(call, &input_kind, &input)) {
			cJSON_Delete(root);
			return MORPH_ERR_PROTOCOL;
		}
		/* Replayed IDs are provider-neutral Morph IDs. */
		snprintf(tool_call->id, sizeof(tool_call->id), "%s", local_id->valuestring);
		snprintf(tool_call->tool_call_id, sizeof(tool_call->tool_call_id), "%s",
			 local_id->valuestring);
		snprintf(tool_call->name, sizeof(tool_call->name), "%s", name->valuestring);
		tool_call->input_kind = input_kind;
		tool_call->arguments = strdup(input);
		if (!tool_call->arguments) {
			cJSON_Delete(root);
			return -ENOMEM;
		}
		int migrate_rc = agent_history_migrate_legacy_tool_call(tool_call);

		if (migrate_rc != 0) {
			cJSON_Delete(root);
			return migrate_rc;
		}
	}
	cJSON_Delete(root);
	return 0;
}

static int history_add_tool_result(const struct model_history_item *item, morph_array_t *messages)
{
	struct chat_message *message = history_push_message(messages);
	const char *call_id = item->tool_call_id ? item->tool_call_id : item->provider_call_id;

	if (!message)
		return -ENOMEM;
	message->role = strdup("tool");
	message->content = strdup(item->content ? item->content : "");
	message->tool_call_id = strdup(call_id ? call_id : "");
	if (!message->role || !message->content || !message->tool_call_id)
		return -ENOMEM;
	return 0;
}

static int history_add_assistant_message(const struct model_history_item *item,
					 morph_array_t *messages)
{
	int rc = history_add_text_message(messages, "assistant", item->content);
	cJSON *payload;
	const cJSON *reasoning;
	struct chat_message *message;

	if (rc != 0 || !item->payload_json || !item->payload_json[0])
		return rc;
	payload = cJSON_ParseWithOpts(item->payload_json, NULL, 1);
	if (!payload)
		return MORPH_ERR_PARSE;
	reasoning = cJSON_GetObjectItemCaseSensitive(payload, "reasoning_content");
	message = morph_array_borrow(messages, messages->nelts - 1);
	if (cJSON_IsString(reasoning)) {
		message->reasoning_content = strdup(reasoning->valuestring);
		if (!message->reasoning_content)
			rc = (-ENOMEM);
	}
	cJSON_Delete(payload);
	return rc;
}

char *history_user_content_create(const struct model_history_item *item)
{
	cJSON *payload = item->payload_json ? cJSON_Parse(item->payload_json) : NULL;
	const cJSON *outcome = cJSON_GetObjectItemCaseSensitive(payload, "turn_outcome");
	const cJSON *status = cJSON_GetObjectItemCaseSensitive(outcome, "status");
	const cJSON *finished = cJSON_GetObjectItemCaseSensitive(outcome, "finished");
	morph_buf_t text MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_buf_init(&text, 256);

	if (rc == 0)
		rc = morph_buf_puts(&text, item->content ? item->content : "");
	if (rc == 0 && cJSON_IsString(status) && cJSON_IsTrue(finished))
		rc = morph_buf_printf(
			&text,
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

int history_item_tokens(const struct model_history_item *item, struct tokenizer *tokenizer,
			const char *text)
{
	if (strcmp(item->kind, "user_message") == 0) {
		char *rendered = history_user_content_create(item);
		int tokens;

		if (!rendered)
			return -ENOMEM;
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
		int internal =
			(cJSON_IsString(task) &&
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

static int history_build_chat_messages(struct morph_log *logger,
				       const struct model_history_item *items,
				       morph_array_t *messages)
{
	const struct model_history_item *item = items;
	int rc;

	if (!messages)
		return -EINVAL;
	while (item) {
		if (strcmp(item->kind, "compaction_summary") == 0) {
			morph_buf_t snapshot MORPH_CLEANUP(morph_buf_cleanup) = {0};

			rc = morph_buf_init(&snapshot, 256);
			if (rc == 0)
				rc = morph_buf_printf(
					&snapshot,
					"Snapshot through history sequence %lld. Later "
					"messages and turn outcomes supersede this snapshot; "
					"its pending work may already be resolved below.\n%s",
					(long long)(item->sequence_no > 0 ? item->sequence_no - 1
									  : 0),
					item->content ? item->content : "");
			if (rc != 0)
				return rc;
			char *reference MORPH_CLEANUP(morph_charp_cleanup) =
				prompt_reference_create(morph_buf_cstr_borrow(&snapshot));

			if (!reference)
				return -ENOMEM;
			rc = history_add_text_message(messages, "assistant", reference);
			if (rc != 0)
				return rc;
			struct chat_message *message =
				morph_array_borrow(messages, messages->nelts - 1);

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
			rc = history_add_tool_calls(logger, item, messages);
		else if (strcmp(item->kind, "tool_result") == 0)
			rc = history_add_tool_result(item, messages);
		else if (strcmp(item->kind, "compaction_summary") == 0)
			rc = 0;
		else if (strcmp(item->kind, "user_message") == 0) {
			char *content = history_user_content_create(item);

			if (!content)
				return -ENOMEM;
			rc = history_add_text_message(messages, "user", content);
			free(content);
		} else if (strcmp(item->kind, "assistant_message") == 0)
			rc = history_add_assistant_message(item, messages);
		else if (strcmp(item->kind, "background_receipt") == 0)
			rc = history_add_text_message(messages, "system", item->content);
		else
			rc = 0;
		if (rc != 0)
			return rc;
		item = item->next;
	}
	return 0;
}

void agent_chat_messages_cleanup(morph_array_t *messages)
{
	if (!messages)
		return;
	for (size_t i = 0; i < messages->nelts; i++)
		chat_message_cleanup(morph_array_borrow(messages, i));
	morph_array_cleanup(messages);
}

int agent_history_messages_copy(struct morph_log *logger, const struct model_history_item *items,
				morph_array_t *messages)
{
	morph_array_t candidate MORPH_CLEANUP(agent_chat_messages_cleanup) = {0};
	int rc;

	if (!messages)
		return -EINVAL;
	if (messages->nelts)
		return -EALREADY;
	if (messages->elts && messages->size != sizeof(struct chat_message))
		return -EINVAL;
	rc = morph_array_init(&candidate, messages->cap ? messages->cap : 64,
			      sizeof(struct chat_message));
	if (rc == 0)
		rc = history_build_chat_messages(logger, items, &candidate);
	if (rc != 0)
		return rc;
	agent_chat_messages_move(messages, &candidate);
	return 0;
}

void agent_chat_messages_move(morph_array_t *out, morph_array_t *source)
{
	if (!out || !source || out == source)
		return;
	agent_chat_messages_cleanup(out);
	*out = *source;
	memset(source, 0, sizeof(*source));
}
