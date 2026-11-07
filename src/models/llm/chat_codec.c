#include "chat_codec.h"
#include "util/log.h"
#include "util/json.h"
#include "util/error.h"
#include "util/arena.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "util/utf8.h"

static struct json *build_simple_messages_json(const char *system_prompt, const char **messages,
					       int n)
{
	struct json *arr;

	arr = json_array();
	if (!arr)
		return NULL;

	if (system_prompt && *system_prompt) {
		struct json *msg = json_object();
		if (!msg) {
			json_free(arr);
			return NULL;
		}
		json_add_string(msg, "role", "system");
		json_add_string(msg, "content", system_prompt);
		json_array_add(arr, msg);
	}

	for (int i = 0; i < n; i++) {
		const char *role = (i % 2 == 0) ? "user" : "assistant";
		struct json *msg = json_object();
		if (!msg) {
			json_free(arr);
			return NULL;
		}
		json_add_string(msg, "role", role);
		json_add_string(msg, "content", messages && messages[i] ? messages[i] : "");
		json_array_add(arr, msg);
	}
	return arr;
}

int llm_chat_encode_body(struct morph_log *logger, struct arena *arena, const char *model_id,
			 const char *system_prompt, const char **messages, int n, int max_tokens,
			 int stream, const char *extra_body_json, const struct llm_adapter *adapter,
			 int disable_thinking, char **out_body, size_t *out_body_len)
{
	struct json *root;
	struct json *msgs;
	char *body;

	if (!arena || !model_id || !out_body || !out_body_len)
		return -EINVAL;
	*out_body = NULL;
	*out_body_len = 0;
	root = json_object();
	msgs = build_simple_messages_json(system_prompt, messages, n);
	if (!root || !msgs) {
		json_free(root);
		json_free(msgs);
		return -ENOMEM;
	}
	json_add_string(root, "model", model_id);
	json_object_add(root, "messages", msgs);
	if (stream)
		json_add_bool(root, "stream", 1);
	if (stream) {
		struct json *opts = json_object();
		if (opts) {
			json_add_bool(opts, "include_usage", 1);
			json_object_add(root, "stream_options", opts);
		}
	}
	json_add_number(root, "max_tokens", max_tokens > 0 ? max_tokens : 4096);
	int rc = llm_codec_add_options(logger, root, extra_body_json);
	if (rc == 0 && disable_thinking && adapter && adapter->disable_thinking)
		rc = adapter->disable_thinking(root);
	if (rc < 0) {
		json_free(root);
		return rc;
	}
	body = json_stringify_arena(arena, root);
	json_free(root);
	if (!body)
		return -ENOMEM;
	*out_body = body;
	*out_body_len = strlen(body);
	return 0;
}

int llm_chat_decode_event(const char *event, const char *data, void *ud)
{
	struct llm_stream_ctx *ctx = ud;
	struct json *root;
	const struct json *choices;
	const struct json *first;
	const struct json *delta;
	const struct json *tool_calls;
	const struct json *item;
	const char *content;
	const char *reasoning;

	(void)event;
	if (!data || !*data)
		return 0;
	if (strcmp(data, "[DONE]") == 0) {
		log_dbg(ctx ? ctx->logger : NULL, "llm_chat_decode_event: [DONE]");
		return 0;
	}
	root = json_parse_prefix(data, NULL, NULL);
	if (!root)
		return 0;
	llm_stream_parse_metadata(ctx, root);
	llm_stream_parse_usage(ctx, root);
	choices = json_member_ci(root, "choices");
	first = json_at(choices, 0);
	if (!first) {
		json_free(root);
		return 0;
	}
	llm_stream_parse_finish_reason(ctx, first);
	delta = json_member_ci(first, "delta");
	if (!delta) {
		const char *text = json_string_value(json_member_ci(first, "text"));
		int rc = 0;

		if (text) {
			llm_stream_append(ctx, text);
			if (ctx->user_cb)
				rc = ctx->user_cb(text, ctx->user_data);
		}
		json_free(root);
		return rc;
	}
	content = json_string_value(json_member_ci(delta, "content"));
	if (content) {
		llm_stream_append(ctx, content);
		int rc = llm_stream_emit(ctx, LLM_STREAM_CONTENT, content);

		if (rc != 0) {
			json_free(root);
			return rc;
		}
	}
	reasoning = json_string_value(json_member_ci(delta, "reasoning_content"));
	if (reasoning) {
		(void)morph_buf_puts(&ctx->reasoning, reasoning);
		int rc = llm_stream_emit(ctx, LLM_STREAM_REASONING, reasoning);

		if (rc != 0) {
			json_free(root);
			return rc;
		}
	}
	tool_calls = json_member_ci(delta, "tool_calls");
	if (!json_is_array_value(tool_calls)) {
		json_free(root);
		return 0;
	}
	json_foreach (item, tool_calls) {
		const struct json *index_item = json_member_ci(item, "index");
		int64_t index = 0;
		struct tool_call *call;
		const struct json *function;
		const char *id;
		const char *name;
		const char *arguments;

		if (index_item && json_read_int(index_item, &index) != 0)
			continue;
		if (index < 0 || index > INT_MAX - 4)
			continue;
		call = llm_stream_ensure_tool_call(ctx, (int)index);
		if (!call)
			continue;
		id = json_string_value(json_member_ci(item, "id"));
		if (id)
			strncpy(call->id, id, sizeof(call->id) - 1);
		function = json_member_ci(item, "function");
		name = json_string_value(json_member_ci(function, "name"));
		if (name)
			strncpy(call->name, name, sizeof(call->name) - 1);
		arguments = json_string_value(json_member_ci(function, "arguments"));
		if (arguments) {
			size_t existing = call->arguments ? strlen(call->arguments) : 0;
			size_t add_len = strlen(arguments);
			char *next = arena_alloc(ctx->arena, existing + add_len + 1);

			if (next) {
				if (call->arguments)
					memcpy(next, call->arguments, existing);
				memcpy(next + existing, arguments, add_len + 1);
				call->arguments = next;
			}
		}
	}
	json_free(root);
	return 0;
}

static char *build_text_tool_arguments_json(const char *input)
{
	struct json *wrapper = json_object();
	char *json;

	if (!wrapper)
		return NULL;
	json_add_string(wrapper, "input", input ? input : "");
	json = json_stringify(wrapper);
	json_free(wrapper);
	return json;
}

static struct json *build_structured_messages_json(const char *system_prompt,
						   const struct chat_message *messages,
						   int msg_count, const struct llm_adapter *adapter,
						   const struct model *model)
{
	struct json *arr = json_array();

	if (system_prompt && *system_prompt) {
		struct json *sys_msg = json_object();
		json_add_string(sys_msg, "role", "system");
		json_add_string(sys_msg, "content", system_prompt);
		json_array_add(arr, sys_msg);
	}

	for (int i = 0; i < msg_count; i++) {
		const struct chat_message *m = &messages[i];
		struct json *msg = json_object();
		json_add_string(msg, "role", m->role);

		if (m->content && *m->content)
			json_add_string(msg, "content", m->content);
		else if (adapter->assistant_tool_content_nullable &&
			 strcmp(m->role, "assistant") == 0 && m->tool_calls)
			json_add_null(msg, "content");
		else if (m->content)
			json_add_string(msg, "content", m->content);
		else
			json_add_string(msg, "content", "");

		if (adapter->add_message_fields && adapter->add_message_fields(model, msg, m) < 0) {
			json_free(msg);
			json_free(arr);
			return NULL;
		}

		if (strcmp(m->role, "assistant") == 0 && m->tool_calls && m->tool_call_count > 0) {
			struct json *tc_arr = json_array();
			for (int j = 0; j < m->tool_call_count; j++) {
				struct json *tc_obj = json_object();
				char *text_arguments = NULL;
				const char *arguments = m->tool_calls[j].arguments
								? m->tool_calls[j].arguments
								: "";

				if (m->tool_calls[j].input_kind == TOOL_INPUT_TEXT) {
					text_arguments = build_text_tool_arguments_json(arguments);
					if (!text_arguments) {
						json_free(tc_obj);
						json_free(tc_arr);
						json_free(msg);
						json_free(arr);
						return NULL;
					}
					arguments = text_arguments;
				}
				json_add_string(tc_obj, "id", m->tool_calls[j].id);
				json_add_string(tc_obj, "type", "function");
				struct json *func = json_object();
				json_add_string(func, "name", m->tool_calls[j].name);
				json_add_string(func, "arguments", arguments);
				free(text_arguments);
				json_object_add(tc_obj, "function", func);
				json_array_add(tc_arr, tc_obj);
			}
			json_object_add(msg, "tool_calls", tc_arr);
		}

		if (strcmp(m->role, "tool") == 0 && m->tool_call_id) {
			json_add_string(msg, "tool_call_id", m->tool_call_id);
		}

		json_array_add(arr, msg);
	}

	return arr;
}

static struct json *build_tools_json(const struct tool_desc *tools, int tool_count)
{
	struct json *arr = json_array();
	for (int i = 0; i < tool_count; i++) {
		struct json *tool = json_object();
		json_add_string(tool, "type", "function");

		struct json *func = json_object();
		json_add_string(func, "name", tools[i].name);
		json_add_string(func, "description", tools[i].description);

		struct json *params = NULL;
		if (tools[i].input_kind == TOOL_INPUT_TEXT) {
			params = json_parse_prefix(
				"{\"type\":\"object\",\"properties\":{"
				"\"input\":{\"type\":\"string\","
				"\"description\":\"Raw text input for this tool.\"}},"
				"\"required\":[\"input\"],"
				"\"additionalProperties\":false}",
				NULL, NULL);
		} else if ((tools[i].input_schema && tools[i].input_schema[0])) {
			params = json_parse_prefix(tools[i].input_schema, NULL, NULL);
		}
		params = llm_codec_schema(params);
		if (params)
			json_object_add(func, "parameters", params);
		else
			json_object_add(func, "parameters",
					json_parse_prefix("{\"type\":\"object\",\"properties\":{}}",
							  NULL, NULL));

		json_object_add(tool, "function", func);
		json_array_add(arr, tool);
	}
	return arr;
}

static struct json *chat_input(const struct model *model, const char *system_prompt,
			       const struct chat_message *messages, int count)
{
	return build_structured_messages_json(system_prompt, messages, count,
					      llm_adapter_resolve(model), model);
}

static void chat_configure(const struct model *model, struct json *request, int tool_count)
{
	const struct llm_adapter *adapter = llm_adapter_resolve(model);
	struct json *options = json_object();

	if (tool_count == 0)
		json_object_remove(request, "tools");
	else if (!adapter->supports_tool_choice || adapter->supports_tool_choice(model))
		json_add_string(request, "tool_choice", "auto");
	if (options) {
		json_add_bool(options, "include_usage", 1);
		json_object_add(request, "stream_options", options);
	}
	json_add_number(request, "max_tokens", model->max_tokens > 0 ? model->max_tokens : 4096);
}

const struct llm_codec *llm_chat_codec(void)
{
	static const struct llm_codec codec = {
		.endpoint = "/chat/completions",
		.input_key = "messages",
		.input = chat_input,
		.tools = build_tools_json,
		.configure = chat_configure,
		.event = llm_chat_decode_event,
	};

	return &codec;
}
