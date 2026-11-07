#include "responses_codec.h"
#include "util/log.h"
#include "util/json.h"
#include "util/error.h"
#include "util/arena.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static struct json *build_responses_tools_json(const struct tool_desc *tools, int tool_count)
{
	struct json *array = json_array();

	if (!array)
		return NULL;
	for (int i = 0; i < tool_count; i++) {
		struct json *tool = json_object();

		if (!tool) {
			json_free(array);
			return NULL;
		}
		if (tools[i].input_kind == TOOL_INPUT_TEXT) {
			json_add_string(tool, "type", "custom");
			json_add_string(tool, "name", tools[i].name);
			json_add_string(tool, "description", tools[i].description);
			if ((tools[i].input_format && tools[i].input_format[0])) {
				struct json *format =
					json_parse_prefix(tools[i].input_format, NULL, NULL);

				if (!format) {
					json_free(tool);
					json_free(array);
					return NULL;
				}
				json_object_add(tool, "format", format);
			}
		} else {
			struct json *params =
				(tools[i].input_schema && tools[i].input_schema[0])
					? json_parse_prefix(tools[i].input_schema, NULL, NULL)
					: NULL;

			params = llm_codec_schema(params);
			if (!params)
				params = json_parse_prefix(TOOL_EMPTY_INPUT_SCHEMA, NULL, NULL);
			json_add_string(tool, "type", "function");
			json_add_string(tool, "name", tools[i].name);
			json_add_string(tool, "description", tools[i].description);
			json_object_add(tool, "parameters", params);
		}
		json_array_add(array, tool);
	}
	return array;
}

static enum tool_input_kind responses_call_kind(const struct chat_message *messages, int before,
						const char *call_id)
{
	for (int i = before - 1; i >= 0; i--) {
		for (int j = 0; j < messages[i].tool_call_count; j++) {
			struct tool_call *call = &messages[i].tool_calls[j];

			if (strcmp(call->id, call_id) == 0 ||
			    strcmp(call->tool_call_id, call_id) == 0)
				return call->input_kind;
		}
	}
	return TOOL_INPUT_JSON;
}

static struct json *build_responses_input_json(const struct chat_message *messages, int msg_count)
{
	struct json *array = json_array();

	if (!array)
		return NULL;
	for (int i = 0; i < msg_count; i++) {
		const struct chat_message *message = &messages[i];

		if (message->tool_calls && message->tool_call_count > 0) {
			if (message->content && message->content[0]) {
				struct json *text = json_object();

				json_add_string(text, "role", "assistant");
				json_add_string(text, "content", message->content);
				json_array_add(array, text);
			}
			for (int j = 0; j < message->tool_call_count; j++) {
				struct tool_call *call = &message->tool_calls[j];
				struct json *item = json_object();

				if (call->input_kind == TOOL_INPUT_TEXT) {
					json_add_string(item, "type", "custom_tool_call");
					json_add_string(item, "input",
							call->arguments ? call->arguments : "");
				} else {
					json_add_string(item, "type", "function_call");
					json_add_string(item, "arguments",
							call->arguments ? call->arguments : "{}");
				}
				json_add_string(item, "call_id", call->id);
				json_add_string(item, "name", call->name);
				json_array_add(array, item);
			}
			continue;
		}
		if (message->tool_call_id && message->tool_call_id[0]) {
			struct json *item = json_object();
			enum tool_input_kind kind =
				responses_call_kind(messages, i, message->tool_call_id);

			json_add_string(item, "type",
					kind == TOOL_INPUT_TEXT ? "custom_tool_call_output"
								: "function_call_output");
			json_add_string(item, "call_id", message->tool_call_id);
			json_add_string(item, "output", message->content ? message->content : "");
			json_array_add(array, item);
			continue;
		}
		struct json *item = json_object();
		json_add_string(item, "role", message->role ? message->role : "user");
		json_add_string(item, "content", message->content ? message->content : "");
		json_array_add(array, item);
	}
	return array;
}

static struct tool_call *responses_find_tool_call(struct llm_stream_ctx *ctx, const char *item_id)
{
	if (item_id && *item_id) {
		for (int i = 0; i < ctx->tool_call_count; i++)
			if (strcmp(ctx->tool_calls[i].id, item_id) == 0)
				return &ctx->tool_calls[i];
	}
	if (ctx->tool_call_count <= 0)
		return NULL;
	return &ctx->tool_calls[ctx->tool_call_count - 1];
}

static int responses_append_call_input(struct llm_stream_ctx *ctx, struct tool_call *call,
				       const char *delta)
{
	size_t old_len;
	size_t add_len;
	char *next;

	if (!call || !delta)
		return 0;
	old_len = call->arguments ? strlen(call->arguments) : 0;
	add_len = strlen(delta);
	next = arena_alloc(ctx->arena, old_len + add_len + 1);
	if (!next)
		return -ENOMEM;
	if (call->arguments)
		memcpy(next, call->arguments, old_len);
	memcpy(next + old_len, delta, add_len + 1);
	call->arguments = next;
	return 0;
}

static int responses_parse_output_item(struct llm_stream_ctx *ctx, const struct json *item,
				       int complete)
{
	const char *type = json_string_value(json_member_ci(item, "type"));
	const char *id = json_string_value(json_member_ci(item, "id"));
	const char *call_id = json_string_value(json_member_ci(item, "call_id"));
	const char *name = json_string_value(json_member_ci(item, "name"));
	const char *input;
	struct tool_call *call;
	enum tool_input_kind kind;

	if (!type)
		return 0;
	if (strcmp(type, "custom_tool_call") == 0) {
		kind = TOOL_INPUT_TEXT;
		input = json_string_value(json_member_ci(item, "input"));
	} else if (strcmp(type, "function_call") == 0) {
		kind = TOOL_INPUT_JSON;
		input = json_string_value(json_member_ci(item, "arguments"));
	} else {
		return 0;
	}
	call = responses_find_tool_call(ctx, id);
	if (!call || (id && call->id[0] && strcmp(call->id, id) != 0))
		call = llm_stream_ensure_tool_call(ctx, ctx->tool_call_count);
	if (!call)
		return -ENOMEM;
	call->input_kind = kind;
	if (id)
		strncpy(call->id, id, sizeof(call->id) - 1);
	if (complete && call_id)
		strncpy(call->id, call_id, sizeof(call->id) - 1);
	if (name)
		strncpy(call->name, name, sizeof(call->name) - 1);
	if (complete && !call->arguments && input)
		call->arguments = arena_strdup(ctx->arena, input);
	return 0;
}

static int llm_responses_sse_event_cb(const char *event, const char *data, void *user_data)
{
	struct llm_stream_ctx *ctx = user_data;
	struct json *root;
	const char *event_type;
	int rc = 0;

	(void)event;
	if (!data || !*data || strcmp(data, "[DONE]") == 0)
		return 0;
	root = json_parse_prefix(data, NULL, NULL);
	if (!root)
		return 0;
	event_type = json_string_value(json_member_ci(root, "type"));
	if (!event_type) {
		json_free(root);
		return 0;
	}
	if (strcmp(event_type, "response.output_text.delta") == 0) {
		const char *delta = json_string_value(json_member_ci(root, "delta"));

		if (delta) {
			llm_stream_append(ctx, delta);
			rc = llm_stream_emit(ctx, LLM_STREAM_CONTENT, delta);
		}
	} else if (strcmp(event_type, "response.reasoning_summary_text.delta") == 0) {
		const char *delta = json_string_value(json_member_ci(root, "delta"));

		if (delta)
			rc = llm_stream_emit(ctx, LLM_STREAM_REASONING, delta);
	} else if (strcmp(event_type, "response.output_item.added") == 0 ||
		   strcmp(event_type, "response.output_item.done") == 0) {
		rc = responses_parse_output_item(ctx, json_member_ci(root, "item"),
						 strcmp(event_type, "response.output_item.done") ==
							 0);
	} else if (strcmp(event_type, "response.custom_tool_call_input.delta") == 0 ||
		   strcmp(event_type, "response.function_call_arguments.delta") == 0) {
		const char *item_id = json_string_value(json_member_ci(root, "item_id"));
		const char *delta = json_string_value(json_member_ci(root, "delta"));
		struct tool_call *call = responses_find_tool_call(ctx, item_id);

		rc = responses_append_call_input(ctx, call, delta);
	} else if (strcmp(event_type, "response.completed") == 0) {
		const struct json *response = json_member_ci(root, "response");

		llm_stream_parse_metadata(ctx, response);
		llm_stream_parse_usage(ctx, response);
		llm_usage_copy(ctx->usage.finish_reason, sizeof(ctx->usage.finish_reason),
			       "completed");
	}
	json_free(root);
	return rc;
}

static struct json *responses_input(const struct model *model, const char *system_prompt,
				    const struct chat_message *messages, int count)
{
	(void)model;
	(void)system_prompt;
	return build_responses_input_json(messages, count);
}

static void responses_configure(const struct model *model, struct json *request, int tool_count)
{
	(void)tool_count;
	json_add_string(request, "tool_choice", "auto");
	json_add_bool(request, "parallel_tool_calls", 0);
	json_add_number(request, "max_output_tokens",
			model->max_tokens > 0 ? model->max_tokens : 4096);
}

const struct llm_codec *llm_responses_codec(void)
{
	static const struct llm_codec codec = {
		.endpoint = "/responses",
		.input_key = "input",
		.system_key = "instructions",
		.input = responses_input,
		.tools = build_responses_tools_json,
		.configure = responses_configure,
		.event = llm_responses_sse_event_cb,
	};

	return &codec;
}
