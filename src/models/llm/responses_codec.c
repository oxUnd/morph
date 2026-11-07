#include "responses_codec.h"
#include "util/log.h"
#include "util/json.h"
#include "util/error.h"
#include "util/cleanup.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static cJSON *build_responses_tools_cjson(const struct tool_desc *tools, int tool_count)
{
	cJSON *array MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateArray();
	if (!array)
		return NULL;
	for (int i = 0; i < tool_count; i++) {
		cJSON *tool MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
		if (!tool)
			return NULL;
		if (tools[i].input_kind == TOOL_INPUT_TEXT) {
			if (!cJSON_AddStringToObject(tool, "type", "custom"))
				return NULL;
			if (tools[i].input_format && tools[i].input_format[0]) {
				cJSON *format MORPH_CLEANUP(morph_json_cleanup) =
					cJSON_Parse(tools[i].input_format);
				if (!format || morph_json_object_take(tool, "format", &format) != 0)
					return NULL;
			}
		} else {
			cJSON *params MORPH_CLEANUP(morph_json_cleanup) =
				tools[i].input_schema && tools[i].input_schema[0]
					? cJSON_Parse(tools[i].input_schema)
					: NULL;
			cJSON *schema MORPH_CLEANUP(morph_json_cleanup) =
				llm_codec_schema_copy(params);
			if (!schema && (cJSON_IsObject(params) || cJSON_IsArray(params)))
				return NULL;
			if (!schema)
				schema = cJSON_Parse(TOOL_EMPTY_INPUT_SCHEMA);
			if (!schema || !cJSON_AddStringToObject(tool, "type", "function") ||
			    morph_json_object_take(tool, "parameters", &schema) != 0)
				return NULL;
		}
		if (!cJSON_AddStringToObject(tool, "name", tools[i].name) ||
		    !cJSON_AddStringToObject(tool, "description",
					     tools[i].description ? tools[i].description : "") ||
		    morph_json_array_take(array, &tool) != 0)
			return NULL;
	}
	cJSON *owned = array;
	array = NULL;
	return owned;
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

static cJSON *build_responses_input_cjson(const struct chat_message *messages, int msg_count)
{
	cJSON *array = cJSON_CreateArray();

	if (!array)
		return NULL;
	for (int i = 0; i < msg_count; i++) {
		const struct chat_message *message = &messages[i];

		if (message->tool_calls && message->tool_call_count > 0) {
			if (message->content && message->content[0]) {
				cJSON *text = cJSON_CreateObject();

				cJSON_AddStringToObject(text, "role", "assistant");
				cJSON_AddStringToObject(text, "content", message->content);
				cJSON_AddItemToArray(array, text);
			}
			for (int j = 0; j < message->tool_call_count; j++) {
				struct tool_call *call = &message->tool_calls[j];
				cJSON *item = cJSON_CreateObject();

				if (call->input_kind == TOOL_INPUT_TEXT) {
					cJSON_AddStringToObject(item, "type", "custom_tool_call");
					cJSON_AddStringToObject(item, "input",
								call->arguments ? call->arguments
										: "");
				} else {
					cJSON_AddStringToObject(item, "type", "function_call");
					cJSON_AddStringToObject(item, "arguments",
								call->arguments ? call->arguments
										: "{}");
				}
				cJSON_AddStringToObject(item, "call_id", call->id);
				cJSON_AddStringToObject(item, "name", call->name);
				cJSON_AddItemToArray(array, item);
			}
			continue;
		}
		if (message->tool_call_id && message->tool_call_id[0]) {
			cJSON *item = cJSON_CreateObject();
			enum tool_input_kind kind =
				responses_call_kind(messages, i, message->tool_call_id);

			cJSON_AddStringToObject(item, "type",
						kind == TOOL_INPUT_TEXT ? "custom_tool_call_output"
									: "function_call_output");
			cJSON_AddStringToObject(item, "call_id", message->tool_call_id);
			cJSON_AddStringToObject(item, "output",
						message->content ? message->content : "");
			cJSON_AddItemToArray(array, item);
			continue;
		}
		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "role", message->role ? message->role : "user");
		cJSON_AddStringToObject(item, "content", message->content ? message->content : "");
		cJSON_AddItemToArray(array, item);
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
	(void)ctx;
	size_t old_len;
	size_t add_len;
	char *next;

	if (!call || !delta)
		return 0;
	old_len = call->arguments ? strlen(call->arguments) : 0;
	add_len = strlen(delta);
	if (add_len > SIZE_MAX - old_len - 1)
		return -EOVERFLOW;
	next = realloc(call->arguments, old_len + add_len + 1);
	if (!next)
		return -ENOMEM;
	memcpy(next + old_len, delta, add_len + 1);
	call->arguments = next;
	return 0;
}

static int responses_parse_output_item(struct llm_stream_ctx *ctx, cJSON *item, int complete)
{
	cJSON *type = cJSON_GetObjectItem(item, "type");
	cJSON *id = cJSON_GetObjectItem(item, "id");
	cJSON *call_id = cJSON_GetObjectItem(item, "call_id");
	cJSON *name = cJSON_GetObjectItem(item, "name");
	cJSON *input;
	struct tool_call *call;
	enum tool_input_kind kind;

	if (!cJSON_IsString(type) || !type->valuestring)
		return 0;
	if (strcmp(type->valuestring, "custom_tool_call") == 0) {
		kind = TOOL_INPUT_TEXT;
		input = cJSON_GetObjectItem(item, "input");
	} else if (strcmp(type->valuestring, "function_call") == 0) {
		kind = TOOL_INPUT_JSON;
		input = cJSON_GetObjectItem(item, "arguments");
	} else {
		return 0;
	}
	call = responses_find_tool_call(ctx, cJSON_GetStringValue(id));
	if (!call || (cJSON_IsString(id) && call->id[0] && strcmp(call->id, id->valuestring) != 0))
		call = llm_stream_tool_call_ensure_borrow(ctx, ctx->tool_call_count);
	if (!call)
		return -ENOMEM;
	call->input_kind = kind;
	if (cJSON_IsString(id) && id->valuestring)
		strncpy(call->id, id->valuestring, sizeof(call->id) - 1);
	if (complete && cJSON_IsString(call_id) && call_id->valuestring)
		strncpy(call->id, call_id->valuestring, sizeof(call->id) - 1);
	if (cJSON_IsString(name) && name->valuestring)
		strncpy(call->name, name->valuestring, sizeof(call->name) - 1);
	if (complete && !call->arguments && cJSON_IsString(input) && input->valuestring) {
		call->arguments = strdup(input->valuestring);
		if (!call->arguments)
			return -ENOMEM;
	}
	return 0;
}

static int llm_responses_sse_event_cb(const char *event, const char *data, void *user_data)
{
	struct llm_stream_ctx *ctx = user_data;
	cJSON *root;
	cJSON *type;
	const char *event_type;
	int rc = 0;

	(void)event;
	if (!data || !*data || strcmp(data, "[DONE]") == 0)
		return 0;
	root = cJSON_Parse(data);
	if (!root)
		return 0;
	type = cJSON_GetObjectItem(root, "type");
	event_type = cJSON_GetStringValue(type);
	if (!event_type) {
		cJSON_Delete(root);
		return 0;
	}
	if (strcmp(event_type, "response.output_text.delta") == 0) {
		const char *delta = cJSON_GetStringValue(cJSON_GetObjectItem(root, "delta"));

		if (delta) {
			llm_stream_append(ctx, delta);
			rc = llm_stream_emit(ctx, LLM_STREAM_CONTENT, delta);
		}
	} else if (strcmp(event_type, "response.reasoning_summary_text.delta") == 0) {
		const char *delta = cJSON_GetStringValue(cJSON_GetObjectItem(root, "delta"));

		if (delta)
			rc = llm_stream_emit(ctx, LLM_STREAM_REASONING, delta);
	} else if (strcmp(event_type, "response.output_item.added") == 0 ||
		   strcmp(event_type, "response.output_item.done") == 0) {
		rc = responses_parse_output_item(ctx, cJSON_GetObjectItem(root, "item"),
						 strcmp(event_type, "response.output_item.done") ==
							 0);
	} else if (strcmp(event_type, "response.custom_tool_call_input.delta") == 0 ||
		   strcmp(event_type, "response.function_call_arguments.delta") == 0) {
		const char *item_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "item_id"));
		const char *delta = cJSON_GetStringValue(cJSON_GetObjectItem(root, "delta"));
		struct tool_call *call = responses_find_tool_call(ctx, item_id);

		rc = responses_append_call_input(ctx, call, delta);
	} else if (strcmp(event_type, "response.completed") == 0) {
		cJSON *response = cJSON_GetObjectItem(root, "response");

		llm_stream_parse_metadata(ctx, response);
		llm_stream_parse_usage(ctx, response);
		llm_usage_copy(ctx->usage.finish_reason, sizeof(ctx->usage.finish_reason),
			       "completed");
	}
	cJSON_Delete(root);
	return rc;
}

static cJSON *responses_input(const struct model *model, const char *system_prompt,
			      const struct chat_message *messages, int count)
{
	(void)model;
	(void)system_prompt;
	return build_responses_input_cjson(messages, count);
}

static void responses_configure(const struct model *model, cJSON *request, int tool_count)
{
	(void)tool_count;
	cJSON_AddStringToObject(request, "tool_choice", "auto");
	cJSON_AddBoolToObject(request, "parallel_tool_calls", 0);
	cJSON_AddNumberToObject(request, "max_output_tokens",
				model->max_tokens > 0 ? model->max_tokens : 4096);
}

const struct llm_codec *llm_responses_codec_borrow(void)
{
	static const struct llm_codec codec = {
		.endpoint = "/responses",
		.input_key = "input",
		.system_key = "instructions",
		.input = responses_input,
		.tools = build_responses_tools_cjson,
		.configure = responses_configure,
		.event = llm_responses_sse_event_cb,
	};

	return &codec;
}
