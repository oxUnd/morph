#include "runtime/mcp_sampling.h"

#include "runtime/mcp_elicitation.h"
#include "agent/tool_runtime.h"
#include "agent/tool_context.h"
#include "models/llm.h"
#include <nb/buf.h>
#include <nb/cleanup.h>
#include <nb/encoding.h>
#include <nb/file.h>
#include <nb/map.h>
#include <nb/str.h>
#include <nb/vec.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__CYGWIN__)
#include <sys/cygwin.h>
#endif

static const char *sampling_text(const cJSON *value, const char *name)
{
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(value, name));

	return text ? text : "";
}

static void sampling_messages_cleanup(nb_vec *messages)
{
	for (size_t i = 0; i < messages->len; i++)
		chat_message_cleanup(nb_vec_at(messages, i));

	nb_vec_destroy(messages);
}

static void sampling_tools_cleanup(nb_vec *tools)
{
	for (size_t i = 0; i < tools->len; i++) {
		struct tool_desc *tool = nb_vec_at(tools, i);
		cJSON_free((void *)tool->input_schema);
	}

	nb_vec_destroy(tools);
}

static cJSON *sampling_parts_create(const cJSON *content)
{
	if (!cJSON_IsArray(content) && !cJSON_IsObject(content))
		return NULL;

	if (cJSON_IsArray(content))
		return cJSON_Duplicate(content, 1);

	cJSON *parts NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateArray();
	cJSON *item NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Duplicate(content, 1);
	if (!parts || !item || !cJSON_AddItemToArray(parts, item))
		return NULL;

	item = NULL;
	cJSON *result = parts;
	parts = NULL;
	return result;
}

static int sampling_content_valid(const cJSON *parts, int *vision)
{
	const cJSON *part;
	cJSON_ArrayForEach (part, parts) {
		const char *type = sampling_text(part, "type");
		if (!strcmp(type, "text")) {
			if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(part, "text")))
				return 0;

			continue;
		}

		if (strcmp(type, "image") && strcmp(type, "audio"))
			return 0;

		const char *mime = sampling_text(part, "mimeType");
		const char *data = sampling_text(part, "data");
		if (strncmp(mime, !strcmp(type, "image") ? "image/" : "audio/", 6) ||
		    strpbrk(mime, "\r\n; ,") || !data[0] ||
		    !nb_base64_valid(nb_str_from_cstr(data), NB_B64_STD))
			return 0;

		if (!strcmp(type, "image"))
			*vision = 1;
	}

	return 1;
}

static struct chat_message *sampling_message_push(nb_vec *messages, const char *role)
{
	struct chat_message *message = nb_vec_push(messages);
	if (!message)
		return NULL;

	memset(message, 0, sizeof(*message));
	message->role = strdup(role);
	return message->role ? message : NULL;
}

static int sampling_messages_create(const cJSON *input, nb_vec *messages, int *vision)
{
	if (!cJSON_IsArray(input) || !input->child)
		return NB_ERR_INVAL;

	nb_map *pending NB_CLEANUP(nb_map_cleanup) = nb_map_new();
	if (!pending)
		return NB_ERR_NOMEM;

	int outstanding = 0;
	const cJSON *item;
	cJSON_ArrayForEach (item, input) {
		const char *role = sampling_text(item, "role");
		if (strcmp(role, "user") && strcmp(role, "assistant"))
			return NB_ERR_INVAL;

		cJSON *parts NB_CLEANUP(cJSON_DeleteOwner) =
			sampling_parts_create(cJSON_GetObjectItemCaseSensitive(item, "content"));
		if (!parts)
			return NB_ERR_INVAL;

		const cJSON *first = parts->child;
		int tool_results = first && !strcmp(sampling_text(first, "type"), "tool_result");
		if (tool_results) {
			if (strcmp(role, "user"))
				return NB_ERR_INVAL;

			const cJSON *part;
			cJSON_ArrayForEach (part, parts) {
				if (strcmp(sampling_text(part, "type"), "tool_result") ||
				    !sampling_text(part, "toolUseId")[0])
					return NB_ERR_INVAL;

				const char *id = sampling_text(part, "toolUseId");
				if (!nb_map_has_cstr(pending, id))
					return NB_ERR_INVAL;

				(void)nb_map_del_cstr(pending, id);
				outstanding--;

				struct chat_message *message =
					sampling_message_push(messages, "tool");
				if (!message)
					return NB_ERR_NOMEM;

				message->tool_call_id = strdup(sampling_text(part, "toolUseId"));
				message->content_parts = sampling_parts_create(
					cJSON_GetObjectItemCaseSensitive(part, "content"));
				if (!message->tool_call_id || !message->content_parts)
					return NB_ERR_NOMEM;

				const cJSON *failed =
					cJSON_GetObjectItemCaseSensitive(part, "isError");
				if (failed && !cJSON_IsBool(failed))
					return NB_ERR_INVAL;

				if (cJSON_IsTrue(failed)) {
					message->content = strdup("Tool execution failed.");
					if (!message->content)
						return NB_ERR_NOMEM;
				}

				if (!sampling_content_valid(message->content_parts, vision))
					return NB_ERR_INVAL;
			}

			if (outstanding)
				return NB_ERR_INVAL;

			continue;
		}

		if (outstanding)
			return NB_ERR_INVAL;

		struct chat_message *message = sampling_message_push(messages, role);
		if (!message)
			return NB_ERR_NOMEM;

		message->content_parts = cJSON_CreateArray();
		if (!message->content_parts)
			return NB_ERR_NOMEM;

		const cJSON *part;
		int call_count = 0;
		cJSON_ArrayForEach (part, parts) {
			const char *type = sampling_text(part, "type");
			if (!strcmp(type, "tool_result"))
				return NB_ERR_INVAL;

			if (!strcmp(type, "tool_use")) {
				if (strcmp(role, "assistant") || !sampling_text(part, "id")[0] ||
				    !sampling_text(part, "name")[0])
					return NB_ERR_INVAL;

				const char *id = sampling_text(part, "id");
				if (nb_map_has_cstr(pending, id))
					return NB_ERR_INVAL;

				if (!nb_map_put_cstr(pending, id, messages, NULL))
					return NB_ERR_NOMEM;

				outstanding++;

				call_count++;
				continue;
			}

			if (!strcmp(type, "image"))
				*vision = 1;

			if (strcmp(type, "text") && strcmp(type, "image") && strcmp(type, "audio"))
				return NB_ERR_INVAL;

			cJSON *copy NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Duplicate(part, 1);
			if (!copy || !cJSON_AddItemToArray(message->content_parts, copy))
				return NB_ERR_NOMEM;

			copy = NULL;
		}

		if (!sampling_content_valid(message->content_parts, vision))
			return NB_ERR_INVAL;

		if (!call_count)
			continue;

		message->tool_calls = calloc((size_t)call_count, sizeof(*message->tool_calls));
		if (!message->tool_calls)
			return NB_ERR_NOMEM;

		message->tool_call_count = call_count;
		int index = 0;
		cJSON_ArrayForEach (part, parts) {
			if (strcmp(sampling_text(part, "type"), "tool_use"))
				continue;

			struct tool_call *call = &message->tool_calls[index++];
			call->id = strdup(sampling_text(part, "id"));
			call->name = strdup(sampling_text(part, "name"));
			const cJSON *input_value = cJSON_GetObjectItemCaseSensitive(part, "input");
			if (!cJSON_IsObject(input_value))
				return NB_ERR_INVAL;

			char *encoded NB_CLEANUP(cJSON_FreeTextOwner) =
				cJSON_PrintUnformatted(input_value);
			call->arguments = encoded ? strdup(encoded) : NULL;
			if (!call->id || !call->name || !call->arguments)
				return NB_ERR_NOMEM;
		}
	}

	return outstanding ? NB_ERR_INVAL : 0;
}

static int sampling_tools_create(const cJSON *input, nb_vec *tools)
{
	if (input && !cJSON_IsArray(input))
		return NB_ERR_INVAL;

	const cJSON *item;
	cJSON_ArrayForEach (item, input) {
		const cJSON *schema = cJSON_GetObjectItemCaseSensitive(item, "inputSchema");
		if (!sampling_text(item, "name")[0] || !cJSON_IsObject(schema) ||
		    strcmp(sampling_text(schema, "type"), "object"))
			return NB_ERR_INVAL;

		struct tool_desc *tool = nb_vec_push(tools);
		if (!tool)
			return NB_ERR_NOMEM;

		memset(tool, 0, sizeof(*tool));
		tool->name = sampling_text(item, "name");
		tool->description = sampling_text(item, "description");
		tool->input_schema = cJSON_PrintUnformatted(schema);
		if (!tool->input_schema)
			return NB_ERR_NOMEM;
	}

	return 0;
}

static int sampling_review_redact(cJSON *node)
{
	if (cJSON_IsObject(node) && cJSON_HasObjectItem(node, "data") &&
	    (!strcmp(sampling_text(node, "type"), "image") ||
	     !strcmp(sampling_text(node, "type"), "audio"))) {
		cJSON_DeleteItemFromObjectCaseSensitive(node, "data");
		if (!cJSON_AddStringToObject(node, "attachment", "Binary content"))
			return NB_ERR_NOMEM;
	}

	for (cJSON *child = node ? node->child : NULL; child; child = child->next) {
		int rc = sampling_review_redact(child);
		if (rc != 0)
			return rc;
	}

	return 0;
}

static int sampling_review(const char *server, const char *message,
			   const struct tool_runtime_context *context)
{
	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) =
		cJSON_Parse("{\"requestedSchema\":{\"type\":\"object\",\"properties\":{}}}");
	cJSON *reply NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	if (!params || !cJSON_AddStringToObject(params, "message", message))
		return NB_ERR_NOMEM;

	int rc = runtime_mcp_elicitation(server, "elicitation/create", params, context, &reply,
					 NULL);
	if (rc != 0)
		return rc;

	const char *action = sampling_text(reply, "action");
	return !strcmp(action, "accept")    ? 0
	       : !strcmp(action, "decline") ? NB_ERR_PERMISSION
					    : NB_ERR_CANCELLED;
}

static void sampling_usage(const struct model_usage *usage, void *opaque)
{
	const struct model *source = opaque;
	struct model_usage copy = *usage;
	(void)snprintf(copy.phase, sizeof(copy.phase), "%s", "mcp_sampling");
	model_report_usage(source, &copy);
}

static int sampling_response_create(const struct model *model, const struct chat_response *response,
				    cJSON **out)
{
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *content NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateArray();
	if (!result || !content || !cJSON_AddStringToObject(result, "role", "assistant") ||
	    !cJSON_AddStringToObject(result, "model", model->model_id) ||
	    !cJSON_AddStringToObject(
		    result, "stopReason",
		    response->tool_call_count				      ? "toolUse"
		    : !strcmp(response->usage.finish_reason, "stop_sequence") ? "stopSequence"
		    : !strcmp(response->usage.finish_reason, "length")	      ? "maxTokens"
									      : "endTurn"))
		return NB_ERR_NOMEM;

	if (response->content && response->content[0]) {
		cJSON *text NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		if (!text || !cJSON_AddStringToObject(text, "type", "text") ||
		    !cJSON_AddStringToObject(text, "text", response->content) ||
		    !cJSON_AddItemToArray(content, text))
			return NB_ERR_NOMEM;

		text = NULL;
	}

	for (int i = 0; i < response->tool_call_count; i++) {
		const struct tool_call *call = &response->tool_calls[i];
		cJSON *tool NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		cJSON *input NB_CLEANUP(cJSON_DeleteOwner) =
			cJSON_Parse(call->arguments ? call->arguments : "{}");
		if (!cJSON_IsObject(input) || !call->id || !call->name)
			return NB_ERR_PROTOCOL;

		if (!tool || !cJSON_AddStringToObject(tool, "type", "tool_use") ||
		    !cJSON_AddStringToObject(tool, "id", call->id) ||
		    !cJSON_AddStringToObject(tool, "name", call->name) ||
		    !cJSON_AddItemToObject(tool, "input", input))
			return NB_ERR_NOMEM;

		input = NULL;
		if (!cJSON_AddItemToArray(content, tool))
			return NB_ERR_NOMEM;

		tool = NULL;
	}

	if (!content->child) {
		cJSON *text NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		if (!text || !cJSON_AddStringToObject(text, "type", "text") ||
		    !cJSON_AddStringToObject(text, "text", "") ||
		    !cJSON_AddItemToArray(content, text))
			return NB_ERR_NOMEM;

		text = NULL;
	}

	if (!cJSON_AddItemToObject(result, "content", content))
		return NB_ERR_NOMEM;

	content = NULL;
	*out = result;
	result = NULL;
	return 0;
}

int runtime_mcp_sampling(const char *server, const char *method, const cJSON *params,
			 const void *opaque, cJSON **result, void *user_data)
{
	(void)method;
	(void)user_data;
	const struct tool_runtime_context *context = opaque;
	if (!context || !context->models.text || !context->ask_user_fn)
		return NB_ERR_CANCELLED;

	nb_vec messages NB_CLEANUP(sampling_messages_cleanup) = {0};
	nb_vec tools NB_CLEANUP(sampling_tools_cleanup) = {0};
	struct chat_message message = {0};
	struct tool_desc tool = {0};
	nb_vec_init(&messages, sizeof(message));
	nb_vec_init(&tools, sizeof(tool));
	int vision = 0;
	int rc = sampling_messages_create(cJSON_GetObjectItemCaseSensitive(params, "messages"),
					  &messages, &vision);
	if (rc == 0)
		rc = sampling_tools_create(cJSON_GetObjectItemCaseSensitive(params, "tools"),
					   &tools);

	const cJSON *limit = cJSON_GetObjectItemCaseSensitive(params, "maxTokens");
	if (rc != 0)
		return rc;

	if (!cJSON_IsNumber(limit) || !isfinite(limit->valuedouble) || limit->valuedouble < 1 ||
	    trunc(limit->valuedouble) != limit->valuedouble || limit->valuedouble > INT_MAX ||
	    messages.len > INT_MAX || tools.len > INT_MAX)
		return NB_ERR_INVAL;

	const cJSON *temperature = cJSON_GetObjectItemCaseSensitive(params, "temperature");
	const cJSON *stops = cJSON_GetObjectItemCaseSensitive(params, "stopSequences");
	const cJSON *tool_choice = cJSON_GetObjectItemCaseSensitive(params, "toolChoice");
	if ((temperature &&
	     (!cJSON_IsNumber(temperature) || !isfinite(temperature->valuedouble))) ||
	    (stops && !cJSON_IsArray(stops)) || (tool_choice && !cJSON_IsObject(tool_choice)))
		return NB_ERR_INVAL;

	const cJSON *stop;
	cJSON_ArrayForEach (stop, stops) {
		if (!cJSON_IsString(stop))
			return NB_ERR_INVAL;
	}

	const struct model *source =
		vision && context->models.vision ? context->models.vision : context->models.text;
	if (!source->chat_with_tools)
		return NB_ERR_UNSUPPORTED;

	const char *choice =
		sampling_text(cJSON_GetObjectItemCaseSensitive(params, "toolChoice"), "mode");
	if (choice[0] && strcmp(choice, "auto") && strcmp(choice, "required") &&
	    strcmp(choice, "none"))
		return NB_ERR_INVAL;

	if (!strcmp(choice, "required") && tools.len == 0)
		return NB_ERR_INVAL;

	nb_buf review NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf_init(&review);
	(void)nb_buf_printf(&review, "Allow a generation using %s?\n\nSystem instructions:\n%s",
			    source->model_id, sampling_text(params, "systemPrompt"));
	int output_limit = (int)limit->valuedouble;
	if (source->max_tokens > 0 && output_limit > source->max_tokens)
		output_limit = source->max_tokens;

	(void)nb_buf_printf(&review, "\n\nMaximum output: %d tokens.", output_limit);
	const cJSON *requested_tools = cJSON_GetObjectItemCaseSensitive(params, "tools");
	if (requested_tools) {
		char *text NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_Print(requested_tools);
		if (!text || !nb_buf_printf(&review, "\n\nAvailable tools:\n%s", text))
			return NB_ERR_NOMEM;
	}

	const cJSON *input_message;
	cJSON_ArrayForEach (input_message, cJSON_GetObjectItemCaseSensitive(params, "messages")) {
		char *text NB_CLEANUP(cJSON_FreeTextOwner) = NULL;
		cJSON *copy NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Duplicate(input_message, 1);
		if (!copy)
			return NB_ERR_NOMEM;

		if (sampling_review_redact(copy) != 0)
			return NB_ERR_NOMEM;

		text = cJSON_Print(copy);
		if (!text || !nb_buf_printf(&review, "\n\n%s", text))
			return NB_ERR_NOMEM;
	}

	if (!nb_buf_ok(&review))
		return nb_buf_status(&review);

	rc = sampling_review(server, nb_buf_cstr(&review), context);
	if (rc != 0)
		return rc;

	struct model view;
	(void)model_request_view_init(source, &context->http, &view);
	view.max_tokens = (int)limit->valuedouble;
	if (source->max_tokens > 0 && view.max_tokens > source->max_tokens)
		view.max_tokens = source->max_tokens;

	cJSON *options NB_CLEANUP(cJSON_DeleteOwner) =
		view.extra_body_json[0] ? cJSON_Parse(view.extra_body_json) : cJSON_CreateObject();
	if (!cJSON_IsObject(options))
		return NB_ERR_INVAL;

	if (temperature) {
		cJSON_DeleteItemFromObjectCaseSensitive(options, "temperature");
		cJSON *copy NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Duplicate(temperature, 1);
		if (!copy || !cJSON_AddItemToObject(options, "temperature", copy))
			return NB_ERR_NOMEM;

		copy = NULL;
	}

	if (choice[0]) {
		cJSON_DeleteItemFromObjectCaseSensitive(options, "tool_choice");
		if (!cJSON_AddStringToObject(options, "tool_choice", choice))
			return NB_ERR_NOMEM;
	}

	char *encoded NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(options);
	if (!encoded || strlen(encoded) >= sizeof(view.extra_body_json))
		return encoded ? NB_ERR_TOO_BIG : NB_ERR_NOMEM;

	memcpy(view.extra_body_json, encoded, strlen(encoded) + 1);
	struct model_usage_sink sink = {.callback = sampling_usage, .user_data = (void *)source};
	view.usage_sink = &sink;
	view.budget_sink = NULL;
	struct chat_response response NB_CLEANUP(chat_response_free) = {0};
	rc = view.chat_with_tools(&view, sampling_text(params, "systemPrompt"),
				  (struct chat_message *)messages.data, (int)messages.len,
				  (struct tool_desc *)tools.data, (int)tools.len, &response, NULL,
				  NULL);
	if (rc != 0)
		return rc;

	/* Apply stop sequences independently of provider-specific request options. */
	char *first_stop = NULL;
	cJSON_ArrayForEach (stop, stops) {
		char *match = response.content ? strstr(response.content, stop->valuestring) : NULL;
		if (match && (!first_stop || match < first_stop))
			first_stop = match;
	}

	if (first_stop) {
		*first_stop = '\0';
		for (int i = 0; i < response.tool_call_count; i++)
			tool_call_cleanup(&response.tool_calls[i]);

		free(response.tool_calls);
		response.tool_calls = NULL;
		response.tool_call_count = 0;
		(void)snprintf(response.usage.finish_reason, sizeof(response.usage.finish_reason),
			       "%s", "stop_sequence");
	}

	if (!strcmp(choice, "required") && !response.tool_call_count)
		return NB_ERR_PROTOCOL;

	cJSON *sampled NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	rc = sampling_response_create(&view, &response, &sampled);
	if (rc != 0)
		return rc;

	char *answer NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_Print(sampled);
	if (!answer)
		return NB_ERR_NOMEM;

	nb_buf_clear(&review);
	if (!nb_buf_printf(&review, "Allow this response to be sent to the MCP server?\n\n%s",
			   answer))
		return NB_ERR_NOMEM;

	rc = sampling_review(server, nb_buf_cstr(&review), context);
	if (rc != 0)
		return rc;

	*result = sampled;
	sampled = NULL;
	return 0;
}

int runtime_mcp_roots(const char *server, const char *method, const cJSON *params,
		      const void *opaque, cJSON **result, void *user_data)
{
	(void)server;
	(void)method;
	(void)params;
	(void)user_data;
	const struct tool_runtime_context *context = opaque;
	cJSON *reply NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!reply)
		return NB_ERR_NOMEM;

	cJSON *roots = cJSON_AddArrayToObject(reply, "roots");
	if (!roots)
		return NB_ERR_NOMEM;

	const char *workdir = context && context->workdir ? context->workdir
			      : user_data		  ? tool_context_workdir_borrow(user_data)
							  : NULL;
	if (workdir && workdir[0]) {
		nb_buf absolute NB_CLEANUP(nb_buf_cleanup) = {0};
		nb_buf uri NB_CLEANUP(nb_buf_cleanup) = {0};
		int rc = nb_path_absolute(&absolute, workdir, NULL);
		if (rc != 0)
			return rc;

#if defined(__CYGWIN__)
		nb_buf native NB_CLEANUP(nb_buf_cleanup) = {0};
		ssize_t needed = cygwin_conv_path(CCP_POSIX_TO_WIN_A | CCP_ABSOLUTE,
						  nb_buf_cstr(&absolute), NULL, 0);
		if (needed < 0)
			return nb_status_from_errno(errno, NULL);

		nb_buf_init(&native);
		if (!nb_buf_reserve(&native, (size_t)needed))
			return NB_ERR_NOMEM;

		if (cygwin_conv_path(CCP_POSIX_TO_WIN_A | CCP_ABSOLUTE, nb_buf_cstr(&absolute),
				     native.ptr, (size_t)needed) != 0)
			return nb_status_from_errno(errno, NULL);

		native.len = strlen(native.ptr);
		nb_buf_move(&absolute, &native);
#endif

		for (size_t i = 0; i < absolute.len; i++) {
			if (absolute.ptr[i] == '\\')
				absolute.ptr[i] = '/';
		}

		int drive = absolute.len >= 3 && absolute.ptr[1] == ':' && absolute.ptr[2] == '/';
		int unc = absolute.len >= 2 && absolute.ptr[0] == '/' && absolute.ptr[1] == '/';

		nb_buf_init(&uri);
		if (!nb_buf_append_cstr(&uri, drive ? "file:///" : unc ? "file:" : "file://"))
			return NB_ERR_NOMEM;

		for (size_t i = 0; i < absolute.len; i++) {
			char byte = absolute.ptr[i];
			if (byte == '/' || (drive && i == 1 && byte == ':')) {
				if (!nb_buf_append_char(&uri, byte))
					return NB_ERR_NOMEM;
			} else {
				rc = nb_url_encode(nb_str_from_parts(&byte, 1), false, &uri, NULL);
				if (rc != 0)
					return rc;
			}
		}

		cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		if (!root || !cJSON_AddStringToObject(root, "uri", nb_buf_cstr(&uri)) ||
		    !cJSON_AddStringToObject(root, "name", "Workspace") ||
		    !cJSON_AddItemToArray(roots, root))
			return NB_ERR_NOMEM;

		root = NULL;
	}

	*result = reply;
	reply = NULL;
	return 0;
}
