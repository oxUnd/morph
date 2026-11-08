#include "chat_codec.h"
#include "util/log.h"
#include "util/json.h"
#include "util/error.h"
#include "util/cleanup.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "util/utf8.h"

static cJSON *build_simple_messages_cjson(const char *system_prompt, const char **messages, int n)
{
	cJSON *arr;

	arr = cJSON_CreateArray();
	if (!arr)
		return NULL;

	if (system_prompt && *system_prompt) {
		cJSON *msg = cJSON_CreateObject();
		if (!msg) {
			cJSON_Delete(arr);
			return NULL;
		}
		cJSON_AddStringToObject(msg, "role", "system");
		cJSON_AddStringToObject(msg, "content", system_prompt);
		cJSON_AddItemToArray(arr, msg);
	}

	for (int i = 0; i < n; i++) {
		const char *role = (i % 2 == 0) ? "user" : "assistant";
		cJSON *msg = cJSON_CreateObject();
		if (!msg) {
			cJSON_Delete(arr);
			return NULL;
		}
		cJSON_AddStringToObject(msg, "role", role);
		cJSON_AddStringToObject(msg, "content", messages && messages[i] ? messages[i] : "");
		cJSON_AddItemToArray(arr, msg);
	}
	return arr;
}

int llm_chat_body_create(struct morph_log *logger, const char *model_id, const char *system_prompt,
			 const char **messages, int n, int max_tokens, int stream,
			 const char *extra_body_json, const struct llm_adapter *adapter,
			 int disable_thinking, char **out_body, size_t *out_body_len)
{
	cJSON *root;
	cJSON *msgs;
	char *body;

	if (!model_id || !out_body || !out_body_len)
		return -EINVAL;
	if (*out_body)
		return -EEXIST;
	root = cJSON_CreateObject();
	msgs = build_simple_messages_cjson(system_prompt, messages, n);
	if (!root || !msgs) {
		cJSON_Delete(root);
		cJSON_Delete(msgs);
		return -ENOMEM;
	}
	cJSON_AddStringToObject(root, "model", model_id);
	cJSON_AddItemToObject(root, "messages", msgs);
	if (stream)
		cJSON_AddBoolToObject(root, "stream", 1);
	if (stream) {
		cJSON *opts = cJSON_CreateObject();
		if (opts) {
			cJSON_AddBoolToObject(opts, "include_usage", 1);
			cJSON_AddItemToObject(root, "stream_options", opts);
		}
	}
	cJSON_AddNumberToObject(root, "max_tokens", max_tokens > 0 ? max_tokens : 4096);
	int rc = llm_codec_add_options(logger, root, extra_body_json);
	if (rc == 0 && disable_thinking && adapter && adapter->disable_thinking)
		rc = adapter->disable_thinking(root);
	if (rc < 0) {
		cJSON_Delete(root);
		return rc;
	}
	body = morph_json_print_copy(root);
	cJSON_Delete(root);
	if (!body)
		return -ENOMEM;
	*out_body = body;
	*out_body_len = strlen(body);
	return 0;
}

int llm_chat_decode_event(const char *event, const char *data, void *ud)
{
	struct llm_stream_ctx *ctx = ud;
	(void)event;
	if (!data || !*data)
		return 0;

	if (strcmp(data, "[DONE]") == 0) {
		ctx->terminal_received = 1;
		log_dbg(ctx ? ctx->logger : NULL, "llm_chat_decode_event: [DONE]");
		return 0;
	}

	cJSON *root = cJSON_Parse(data);
	if (!root)
		return 0;

	llm_stream_parse_metadata(ctx, root);
	llm_stream_parse_usage(ctx, root);

	cJSON *choices = cJSON_GetObjectItem(root, "choices");
	if (!cJSON_IsArray(choices)) {
		cJSON_Delete(root);
		return 0;
	}

	cJSON *first = cJSON_GetArrayItem(choices, 0);
	if (!first) {
		cJSON_Delete(root);
		return 0;
	}
	llm_stream_parse_finish_reason(ctx, first);

	cJSON *delta = cJSON_GetObjectItem(first, "delta");
	if (!delta) {
		cJSON *text_item = cJSON_GetObjectItem(first, "text");
		if (cJSON_IsString(text_item) && text_item->valuestring) {
			llm_stream_append(ctx, text_item->valuestring);
			if (ctx->user_cb) {
				int rc = ctx->user_cb(text_item->valuestring, ctx->user_data);
				if (rc != 0) {
					cJSON_Delete(root);
					return rc;
				}
			}
		}
		cJSON_Delete(root);
		return 0;
	}

	cJSON *content = cJSON_GetObjectItem(delta, "content");
	if (cJSON_IsString(content) && content->valuestring) {
		llm_stream_append(ctx, content->valuestring);
		int rc = llm_stream_emit(ctx, LLM_STREAM_CONTENT, content->valuestring);
		if (rc != 0) {
			cJSON_Delete(root);
			return rc;
		}
	}

	cJSON *reasoning = cJSON_GetObjectItem(delta, "reasoning_content");
	if (cJSON_IsString(reasoning) && reasoning->valuestring) {
		(void)morph_buf_puts(&ctx->reasoning, reasoning->valuestring);
		int rc = llm_stream_emit(ctx, LLM_STREAM_REASONING, reasoning->valuestring);
		if (rc != 0) {
			cJSON_Delete(root);
			return rc;
		}
	}

	cJSON *tool_calls_arr = cJSON_GetObjectItem(delta, "tool_calls");
	if (cJSON_IsArray(tool_calls_arr)) {
		cJSON *tc_item;
		cJSON_ArrayForEach (tc_item, tool_calls_arr) {
			cJSON *idx_item = cJSON_GetObjectItem(tc_item, "index");
			int index = cJSON_IsNumber(idx_item) ? (int)idx_item->valuedouble : 0;

			struct tool_call *tc = llm_stream_tool_call_ensure_borrow(ctx, index);
			if (!tc) {
				cJSON_Delete(root);
				return -ENOMEM;
			}

			cJSON *id_item = cJSON_GetObjectItem(tc_item, "id");
			if (cJSON_IsString(id_item) && id_item->valuestring) {
				int rc = llm_stream_string_copy(&tc->id, id_item->valuestring);
				if (rc != 0) {
					cJSON_Delete(root);
					return rc;
				}
			}

			cJSON *func_obj = cJSON_GetObjectItem(tc_item, "function");
			if (func_obj) {
				cJSON *name_item = cJSON_GetObjectItem(func_obj, "name");
				if (cJSON_IsString(name_item) && name_item->valuestring) {
					int rc = llm_stream_string_copy(
						&tc->name, name_item->valuestring);
					if (rc != 0) {
						cJSON_Delete(root);
						return rc;
					}
				}
				cJSON *args_item = cJSON_GetObjectItem(func_obj, "arguments");
				if (cJSON_IsString(args_item) && args_item->valuestring) {
					size_t existing = tc->arguments ? strlen(tc->arguments) : 0;
					size_t add_len = strlen(args_item->valuestring);
					if (add_len > SIZE_MAX - existing - 1) {
						cJSON_Delete(root);
						return -EOVERFLOW;
					}
					char *new_args =
						realloc(tc->arguments, existing + add_len + 1);
					if (!new_args) {
						cJSON_Delete(root);
						return -ENOMEM;
					}
					{
						tc->arguments = new_args;
						memcpy(tc->arguments + existing,
						       args_item->valuestring, add_len);
						tc->arguments[existing + add_len] = '\0';
					}
				}
			}
		}
	}

	cJSON_Delete(root);
	return 0;
}

static char *build_text_tool_arguments_json(const char *input)
{
	cJSON *wrapper = cJSON_CreateObject();
	char *json;

	if (!wrapper)
		return NULL;
	cJSON_AddStringToObject(wrapper, "input", input ? input : "");
	json = cJSON_PrintUnformatted(wrapper);
	cJSON_Delete(wrapper);
	return json;
}

static cJSON *build_structured_messages_cjson(const char *system_prompt,
					      const struct chat_message *messages, int msg_count,
					      const struct llm_adapter *adapter,
					      const struct model *model)
{
	cJSON *arr = cJSON_CreateArray();

	if (system_prompt && *system_prompt) {
		cJSON *sys_msg = cJSON_CreateObject();
		cJSON_AddStringToObject(sys_msg, "role", "system");
		cJSON_AddStringToObject(sys_msg, "content", system_prompt);
		cJSON_AddItemToArray(arr, sys_msg);
	}

	for (int i = 0; i < msg_count; i++) {
		const struct chat_message *m = &messages[i];
		cJSON *msg = cJSON_CreateObject();
		cJSON_AddStringToObject(msg, "role", m->role);

		if (m->content && *m->content)
			cJSON_AddStringToObject(msg, "content", m->content);
		else if (adapter->assistant_tool_content_nullable &&
			 strcmp(m->role, "assistant") == 0 && m->tool_calls)
			cJSON_AddNullToObject(msg, "content");
		else if (m->content)
			cJSON_AddStringToObject(msg, "content", m->content);
		else
			cJSON_AddStringToObject(msg, "content", "");

		if (adapter->add_message_fields && adapter->add_message_fields(model, msg, m) < 0) {
			cJSON_Delete(msg);
			cJSON_Delete(arr);
			return NULL;
		}

		if (strcmp(m->role, "assistant") == 0 && m->tool_calls && m->tool_call_count > 0) {
			cJSON *tc_arr = cJSON_CreateArray();
			for (int j = 0; j < m->tool_call_count; j++) {
				cJSON *tc_obj = cJSON_CreateObject();
				char *text_arguments = NULL;
				const char *arguments = m->tool_calls[j].arguments
								? m->tool_calls[j].arguments
								: "";

				if (m->tool_calls[j].input_kind == TOOL_INPUT_TEXT) {
					text_arguments = build_text_tool_arguments_json(arguments);
					if (!text_arguments) {
						cJSON_Delete(tc_obj);
						cJSON_Delete(tc_arr);
						cJSON_Delete(msg);
						cJSON_Delete(arr);
						return NULL;
					}
					arguments = text_arguments;
				}
				cJSON_AddStringToObject(tc_obj, "id", m->tool_calls[j].id);
				cJSON_AddStringToObject(tc_obj, "type", "function");
				cJSON *func = cJSON_CreateObject();
				cJSON_AddStringToObject(func, "name", m->tool_calls[j].name);
				cJSON_AddStringToObject(func, "arguments", arguments);
				cJSON_free(text_arguments);
				cJSON_AddItemToObject(tc_obj, "function", func);
				cJSON_AddItemToArray(tc_arr, tc_obj);
			}
			cJSON_AddItemToObject(msg, "tool_calls", tc_arr);
		}

		if (strcmp(m->role, "tool") == 0 && m->tool_call_id) {
			cJSON_AddStringToObject(msg, "tool_call_id", m->tool_call_id);
		}

		cJSON_AddItemToArray(arr, msg);
	}

	return arr;
}

static cJSON *build_tools_cjson(const struct tool_desc *tools, int tool_count)
{
	cJSON *array MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateArray();
	if (!array)
		return NULL;
	for (int i = 0; i < tool_count; i++) {
		cJSON *tool MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
		cJSON *function MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
		cJSON *params MORPH_CLEANUP(morph_json_cleanup) = NULL;
		if (!tool || !function || !cJSON_AddStringToObject(tool, "type", "function") ||
		    !cJSON_AddStringToObject(function, "name", tools[i].name) ||
		    !cJSON_AddStringToObject(function, "description",
					     tools[i].description ? tools[i].description : ""))
			return NULL;
		if (tools[i].input_kind == TOOL_INPUT_TEXT) {
			params = cJSON_Parse(
				"{\"type\":\"object\",\"properties\":{"
				"\"input\":{\"type\":\"string\","
				"\"description\":\"Raw text input for this tool.\"}},"
				"\"required\":[\"input\"],\"additionalProperties\":false}");
			if (!params)
				return NULL;
		} else if (tools[i].input_schema && tools[i].input_schema[0]) {
			params = cJSON_Parse(tools[i].input_schema);
		}
		cJSON *schema MORPH_CLEANUP(morph_json_cleanup) = llm_codec_schema_copy(params);
		if (!schema && (cJSON_IsObject(params) || cJSON_IsArray(params)))
			return NULL;
		if (!schema)
			schema = cJSON_Parse("{\"type\":\"object\",\"properties\":{}}");
		if (!schema || morph_json_object_take(function, "parameters", &schema) != 0 ||
		    morph_json_object_take(tool, "function", &function) != 0 ||
		    morph_json_array_take(array, &tool) != 0)
			return NULL;
	}
	cJSON *owned = array;
	array = NULL;
	return owned;
}

static cJSON *chat_input(const struct model *model, const char *system_prompt,
			 const struct chat_message *messages, int count)
{
	return build_structured_messages_cjson(system_prompt, messages, count,
					       llm_adapter_borrow(model), model);
}

static void chat_configure(const struct model *model, cJSON *request, int tool_count)
{
	const struct llm_adapter *adapter = llm_adapter_borrow(model);
	cJSON *options = cJSON_CreateObject();

	if (tool_count == 0)
		cJSON_DeleteItemFromObjectCaseSensitive(request, "tools");
	else if (!adapter->supports_tool_choice || adapter->supports_tool_choice(model))
		cJSON_AddStringToObject(request, "tool_choice", "auto");
	if (options) {
		cJSON_AddBoolToObject(options, "include_usage", 1);
		cJSON_AddItemToObject(request, "stream_options", options);
	}
	cJSON_AddNumberToObject(request, "max_tokens",
				model->max_tokens > 0 ? model->max_tokens : 4096);
}

const struct llm_codec *llm_chat_codec_borrow(void)
{
	static const struct llm_codec codec = {
		.endpoint = "/chat/completions",
		.input_key = "messages",
		.input = chat_input,
		.tools = build_tools_cjson,
		.configure = chat_configure,
		.event = llm_chat_decode_event,
		.requires_terminal_event = 1,
	};

	return &codec;
}
