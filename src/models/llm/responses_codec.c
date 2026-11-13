#include "responses_codec.h"
#include "nb/log.h"
#include "cJSON.h"
#include "errors.h"
#include "nb/cleanup.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static cJSON *build_responses_tools_cjson(const struct tool_desc *tools, int tool_count)
{
	cJSON *array NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateArray();
	if (!array)
		return NULL;
	for (int i = 0; i < tool_count; i++) {
		cJSON *tool NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		if (!tool)
			return NULL;
		if (tools[i].input_kind == TOOL_INPUT_TEXT) {
			if (!cJSON_AddStringToObject(tool, "type", "custom"))
				return NULL;
			if (tools[i].input_format && tools[i].input_format[0]) {
				cJSON *format NB_CLEANUP(cJSON_DeleteOwner) =
					cJSON_Parse(tools[i].input_format);
				if (!format || (cJSON_AddItemToObject(tool, "format", format)
							? (format = NULL, NB_OK)
							: NB_ERR_NOMEM) != 0)
					return NULL;
			}
		} else {
			cJSON *params NB_CLEANUP(cJSON_DeleteOwner) =
				tools[i].input_schema && tools[i].input_schema[0]
					? cJSON_Parse(tools[i].input_schema)
					: NULL;
			cJSON *schema NB_CLEANUP(cJSON_DeleteOwner) = llm_codec_schema_copy(params);
			if (!schema && (cJSON_IsObject(params) || cJSON_IsArray(params)))
				return NULL;
			if (!schema)
				schema = cJSON_Parse(TOOL_EMPTY_INPUT_SCHEMA);
			if (!schema || !cJSON_AddStringToObject(tool, "type", "function") ||
			    (cJSON_AddItemToObject(tool, "parameters", schema)
				     ? (schema = NULL, NB_OK)
				     : NB_ERR_NOMEM) != 0)
				return NULL;
		}
		if (!cJSON_AddStringToObject(tool, "name", tools[i].name) ||
		    !cJSON_AddStringToObject(tool, "description",
					     tools[i].description ? tools[i].description : "") ||
		    (cJSON_AddItemToArray(array, tool) ? (tool = NULL, NB_OK) : NB_ERR_NOMEM) != 0)
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

				if ((call->id && strcmp(call->id, call_id) == 0) ||
				    (call->tool_call_id &&
				     strcmp(call->tool_call_id, call_id) == 0))
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
			if (ctx->tool_calls[i].provider_item_id &&
			    strcmp(ctx->tool_calls[i].provider_item_id, item_id) == 0)
				return &ctx->tool_calls[i];
		return NULL;
	}
	if (ctx->tool_call_count != 1)
		return NULL;
	return &ctx->tool_calls[0];
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
	if (!call)
		call = llm_stream_tool_call_ensure_borrow(ctx, ctx->tool_call_count);
	if (!call)
		return NB_ERR_NOMEM;
	call->input_kind = kind;
	int rc = 0;
	if (cJSON_IsString(id) && id->valuestring) {
		rc = llm_stream_string_copy(&call->provider_item_id, id->valuestring);
		if (rc == 0 && !call->id)
			rc = llm_stream_string_copy(&call->id, id->valuestring);
	}
	if (rc == 0 && cJSON_IsString(call_id) && call_id->valuestring)
		rc = llm_stream_string_copy(&call->id, call_id->valuestring);
	if (rc == 0 && cJSON_IsString(name) && name->valuestring)
		rc = llm_stream_string_copy(&call->name, name->valuestring);
	if (rc != 0)
		return rc;
	if (complete && cJSON_IsString(input) && input->valuestring)
		return llm_stream_call_input_set(ctx, call, input->valuestring);
	return 0;
}

static int responses_terminal(struct llm_stream_ctx *ctx, cJSON *root, const char *event_type)
{
	cJSON *response = cJSON_GetObjectItem(root, "response");
	const char *reason = cJSON_GetStringValue(cJSON_GetObjectItem(
		cJSON_GetObjectItem(response, "incomplete_details"), "reason"));
	const cJSON *error = cJSON_GetObjectItem(response, "error");
	const char *status = cJSON_GetStringValue(cJSON_GetObjectItem(response, "status"));
	const char *detail;
	int inconsistent = status &&
		((!strcmp(event_type, "response.completed") && strcmp(status, "completed")) ||
		 (!strcmp(event_type, "response.incomplete") && strcmp(status, "incomplete")));

	ctx->terminal_received = 1;
	llm_stream_parse_metadata(ctx, response);
	llm_stream_parse_usage(ctx, response);
	if (!inconsistent && !strcmp(event_type, "response.completed") &&
	    cJSON_IsObject(response)) {
		llm_usage_copy(ctx->usage.finish_reason, sizeof(ctx->usage.finish_reason),
			       "completed");
		return 0;
	}
	if (!inconsistent && !strcmp(event_type, "response.incomplete") && reason &&
	    !strcmp(reason, "max_output_tokens")) {
		llm_usage_copy(ctx->usage.finish_reason, sizeof(ctx->usage.finish_reason),
			       "length");
		return 0;
	}
	if (!error)
		error = cJSON_GetObjectItem(root, "error");
	detail = cJSON_GetStringValue(cJSON_GetObjectItem(error, "message"));
	if (!detail)
		detail = cJSON_GetStringValue(cJSON_GetObjectItem(root, "message"));
	if (inconsistent)
		detail = "Model terminal event has inconsistent response status.";
	if (!detail)
		detail = reason ? reason : "Model stream ended without a complete response.";
	ctx->terminal_error = !strcmp(event_type, "response.failed") || !strcmp(event_type, "error")
				      ? MORPH_ERR_API
				      : NB_ERR_PROTOCOL;
	llm_usage_copy(ctx->usage.finish_reason, sizeof(ctx->usage.finish_reason),
		      !strcmp(event_type, "response.incomplete") ? "incomplete" : "failed");
	return llm_stream_string_copy(&ctx->terminal_detail, detail);
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
	if (ctx->terminal_received)
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

		rc = llm_stream_call_input_append(ctx, call, delta);
	} else if (!strcmp(event_type, "response.function_call_arguments.done") ||
		   !strcmp(event_type, "response.custom_tool_call_input.done")) {
		const char *item_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "item_id"));
		const char *input = cJSON_GetStringValue(cJSON_GetObjectItem(root,
			!strcmp(event_type, "response.function_call_arguments.done")
				? "arguments" : "input"));
		struct tool_call *call = responses_find_tool_call(ctx, item_id);

		rc = call && input ? llm_stream_call_input_set(ctx, call, input) : NB_ERR_PROTOCOL;
	} else if (!strcmp(event_type, "response.completed") ||
		   !strcmp(event_type, "response.incomplete") ||
		   !strcmp(event_type, "response.failed") || !strcmp(event_type, "error")) {
		rc = responses_terminal(ctx, root, event_type);
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
		.requires_terminal_event = 1,
	};

	return &codec;
}
