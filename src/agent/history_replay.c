#include "history_internal.h"

#include "prompt_context.h"
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

static struct chat_message *history_push_message(morph_array_t *messages)
{
	struct chat_message *message = morph_array_push(messages);

	if (message)
		memset(message, 0, sizeof(*message));
	return message;
}

static int history_add_text_message(morph_array_t *messages, struct arena *arena, const char *role,
				    const char *content)
{
	struct chat_message *message = history_push_message(messages);

	if (!message)
		return -ENOMEM;
	message->role = arena_strdup(arena, role ? role : "user");
	message->content = arena_strdup(arena, content ? content : "");
	if (!message->role || !message->content)
		return -ENOMEM;
	return 0;
}

static int history_tool_arguments_valid(const struct json *arguments)
{
	struct json *parsed;
	int valid;

	if (!json_is_string_value(arguments) || !json_string_value(arguments))
		return 0;
	parsed = json_parse_ex(json_string_value(arguments), NULL, 1, NULL);
	valid = json_is_object_value(parsed);
	json_free(parsed);
	return valid;
}

int history_tool_call_input(const struct json *call, enum tool_input_kind *kind, const char **input)
{
	struct json *kind_item;
	struct json *value;

	if (!json_is_object_value(call) || !kind || !input)
		return 0;
	kind_item = json_member_ci(call, "input_kind");
	if (json_is_string_value(kind_item) && json_string_value(kind_item) &&
	    strcmp(json_string_value(kind_item), "text") == 0) {
		value = json_member_ci(call, "input");
		if (!json_is_string_value(value) || !json_string_value(value))
			return 0;
		*kind = TOOL_INPUT_TEXT;
		*input = json_string_value(value);
		return 1;
	}
	value = json_member_ci(call, "arguments");
	if (!history_tool_arguments_valid(value))
		return 0;
	*kind = TOOL_INPUT_JSON;
	*input = json_string_value(value);
	return 1;
}

int agent_history_normalize_tool_arguments(const char *arguments, char **normalized)
{
	const char *raw = arguments ? arguments : "{}";
	struct json *parsed;
	struct json *fallback;
	char *json;

	if (!normalized)
		return -EINVAL;
	*normalized = NULL;
	parsed = json_parse_ex(raw, NULL, 1, NULL);
	if (json_is_object_value(parsed)) {
		json_free(parsed);
		*normalized = strdup(raw);
		if (!*normalized)
			return -ENOMEM;
		return 0;
	}
	json_free(parsed);
	fallback = json_object();
	if (!fallback)
		return -ENOMEM;
	json_add_string(fallback, "_morph_invalid_arguments", raw);
	json_add_string(fallback, "_morph_recovery",
			"Reissue this tool call with a valid JSON object.");
	json = json_stringify(fallback);
	json_free(fallback);
	if (!json)
		return -ENOMEM;
	*normalized = json;
	return 0;
}

int agent_history_migrate_legacy_tool_call(struct tool_call *call, struct arena *arena)
{
	struct json *arguments;
	char *normalized;

	if (!call || strcmp(call->name, "bash_exec") != 0)
		return 0;
	snprintf(call->name, sizeof(call->name), "%s", "exec");
	if (call->input_kind != TOOL_INPUT_JSON || !call->arguments)
		return 0;
	arguments = json_parse_prefix(call->arguments, NULL, NULL);
	if (!json_is_object_value(arguments)) {
		json_free(arguments);
		return 0;
	}
	json_object_remove_ci(arguments, "timeout_seconds");
	json_object_remove_ci(arguments, "cwd");
	json_object_remove_ci(arguments, "write_paths");
	normalized = json_stringify(arguments);
	json_free(arguments);
	if (!normalized)
		return -ENOMEM;
	if (arena) {
		char *copy = arena_strdup(arena, normalized);

		free(normalized);
		if (!copy)
			return -ENOMEM;
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
	struct json *calls;
	struct json *root;
	int count;

	root = item->payload_json ? json_parse_ex(item->payload_json, NULL, 1, NULL) : NULL;
	if (!root) {
		log_err(logger, "history replay: invalid tool payload at item %lld, sequence %lld",
			(long long)item->id, (long long)item->sequence_no);
		return MORPH_ERR_PARSE;
	}
	calls = json_member_ci(root, "calls");
	count = json_is_array_value(calls) ? json_count(calls) : 0;
	if (count <= 0) {
		json_free(root);
		return MORPH_ERR_PROTOCOL;
	}
	message = history_push_message(messages);
	if (!message) {
		json_free(root);
		return -ENOMEM;
	}
	message->role = arena_strdup(arena, "assistant");
	message->content =
		item->content && item->content[0] ? arena_strdup(arena, item->content) : NULL;
	struct json *reasoning = json_member_ci(root, "reasoning_content");
	if (json_is_string_value(reasoning) && json_string_value(reasoning))
		message->reasoning_content = arena_strdup(arena, json_string_value(reasoning));
	message->tool_calls = arena_alloc(arena, (size_t)count * sizeof(*message->tool_calls));
	if (!message->role || !message->tool_calls ||
	    (json_is_string_value(reasoning) && !message->reasoning_content)) {
		json_free(root);
		return -ENOMEM;
	}
	memset(message->tool_calls, 0, (size_t)count * sizeof(*message->tool_calls));
	message->tool_call_count = count;
	for (int i = 0; i < count; i++) {
		struct json *call = json_at(calls, i);
		struct json *provider_id = json_member_ci(call, "provider_call_id");
		struct json *local_id = json_member_ci(call, "tool_call_id");
		struct json *name = json_member_ci(call, "name");
		struct tool_call *tool_call = &message->tool_calls[i];
		const char *input;
		enum tool_input_kind input_kind;

		if (!json_is_string_value(provider_id) || !json_is_string_value(local_id) ||
		    !json_is_string_value(name) ||
		    !history_tool_call_input(call, &input_kind, &input)) {
			json_free(root);
			return MORPH_ERR_PROTOCOL;
		}
		/* Replayed IDs are provider-neutral Morph IDs. */
		snprintf(tool_call->id, sizeof(tool_call->id), "%s", json_string_value(local_id));
		snprintf(tool_call->tool_call_id, sizeof(tool_call->tool_call_id), "%s",
			 json_string_value(local_id));
		snprintf(tool_call->name, sizeof(tool_call->name), "%s", json_string_value(name));
		tool_call->input_kind = input_kind;
		tool_call->arguments = arena_strdup(arena, input);
		if (!tool_call->arguments) {
			json_free(root);
			return -ENOMEM;
		}
		int migrate_rc = agent_history_migrate_legacy_tool_call(tool_call, arena);

		if (migrate_rc != 0) {
			json_free(root);
			return migrate_rc;
		}
	}
	json_free(root);
	return 0;
}

static int history_add_tool_result(const struct model_history_item *item, morph_array_t *messages,
				   struct arena *arena)
{
	struct chat_message *message = history_push_message(messages);
	const char *call_id = item->tool_call_id ? item->tool_call_id : item->provider_call_id;

	if (!message)
		return -ENOMEM;
	message->role = arena_strdup(arena, "tool");
	message->content = arena_strdup(arena, item->content ? item->content : "");
	message->tool_call_id = arena_strdup(arena, call_id ? call_id : "");
	if (!message->role || !message->content || !message->tool_call_id)
		return -ENOMEM;
	return 0;
}

static int history_add_assistant_message(const struct model_history_item *item,
					 morph_array_t *messages, struct arena *arena)
{
	int rc = history_add_text_message(messages, arena, "assistant", item->content);
	struct json *payload;
	const struct json *reasoning;
	struct chat_message *message;

	if (rc != 0 || !item->payload_json || !item->payload_json[0])
		return rc;
	payload = json_parse_ex(item->payload_json, NULL, 1, NULL);
	if (!payload)
		return MORPH_ERR_PARSE;
	reasoning = json_member(payload, "reasoning_content");
	message = morph_array_get(messages, messages->nelts - 1);
	if (json_is_string_value(reasoning)) {
		message->reasoning_content = arena_strdup(arena, json_string_value(reasoning));
		if (!message->reasoning_content)
			rc = (-ENOMEM);
	}
	json_free(payload);
	return rc;
}

char *history_user_content(const struct model_history_item *item)
{
	struct json *payload =
		item->payload_json ? json_parse_prefix(item->payload_json, NULL, NULL) : NULL;
	const struct json *outcome = json_member(payload, "turn_outcome");
	const struct json *status = json_member(outcome, "status");
	const struct json *finished = json_member(outcome, "finished");
	morph_buf_t text;
	int rc = morph_buf_init(&text, 256);

	if (rc == 0)
		rc = morph_buf_puts(&text, item->content ? item->content : "");
	if (rc == 0 && json_is_string_value(status) && json_is_true(finished))
		rc = morph_buf_printf(
			&text,
			"\n\n[Historical turn execution ended: %s. This records "
			"execution status, not completion of a broader objective. "
			"Later results supersede earlier checkpoint TODOs for this turn.]",
			json_string_value(status));
	json_free(payload);
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
		char *rendered = history_user_content(item);
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
		struct json *payload = item->payload_json
					       ? json_parse_prefix(item->payload_json, NULL, NULL)
					       : NULL;
		const struct json *task = json_member(payload, "task");
		int internal =
			(json_is_string_value(task) &&
			 strcmp(json_string_value(task), "memory_consolidation") == 0) ||
			(item->content && strncmp(item->content, prefix, sizeof(prefix) - 1) == 0);

		json_free(payload);
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
		return -EINVAL;
	while (item) {
		if (strcmp(item->kind, "compaction_summary") == 0) {
			morph_buf_t snapshot;

			rc = morph_buf_init_arena(&snapshot, arena, 256);
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
			char *reference = prompt_reference_build(arena, morph_buf_cstr(&snapshot));

			if (!reference)
				return -ENOMEM;
			rc = history_add_text_message(messages, arena, "assistant", reference);
			if (rc != 0)
				return rc;
			struct chat_message *message =
				morph_array_get(messages, messages->nelts - 1);

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
				return -ENOMEM;
			rc = history_add_text_message(messages, arena, "user", content);
			free(content);
		} else if (strcmp(item->kind, "assistant_message") == 0)
			rc = history_add_assistant_message(item, messages, arena);
		else if (strcmp(item->kind, "background_receipt") == 0)
			rc = history_add_text_message(messages, arena, "system", item->content);
		else
			rc = 0;
		if (rc != 0)
			return rc;
		item = item->next;
	}
	return 0;
}
