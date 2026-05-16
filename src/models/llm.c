#include "llm.h"
#include "agent/tool.h"
#include "util/log.h"
#include "util/file.h"
#include "util/utf8.h"
#include "util/arena.h"
#include "http/client.h"
#include "http/sse.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static char *escape_json_string(struct arena *arena, const char *s)
{
	if (!s)
		return arena_strdup(arena, "");
	size_t len = strlen(s);

	char *clean = arena_alloc(arena, len + 1);
	if (!clean)
		return NULL;
	size_t clean_len = utf8_sanitize_into(clean, s, len);
	clean[clean_len] = '\0';

	size_t cap = clean_len * 2 + 1;
	char *out = arena_alloc(arena, cap);
	if (!out) {
		return NULL;
	}
	size_t j = 0;
	for (size_t i = 0; i < clean_len; i++) {
		if (j + 8 >= cap) {
			cap *= 2;
			char *new_out = arena_alloc(arena, cap);
			if (!new_out) {
				return NULL;
			}
			memcpy(new_out, out, j);
			out = new_out;
		}
		unsigned char c = (unsigned char)clean[i];
		if (c < 0x20) {
			switch (c) {
			case '\n': out[j++] = '\\'; out[j++] = 'n'; break;
			case '\r': out[j++] = '\\'; out[j++] = 'r'; break;
			case '\t': out[j++] = '\\'; out[j++] = 't'; break;
			case '\b': out[j++] = '\\'; out[j++] = 'b'; break;
			case '\f': out[j++] = '\\'; out[j++] = 'f'; break;
			default:
				j += (size_t)snprintf(out + j, cap - j,
						      "\\u%04x", c);
				break;
			}
		} else if (c == '"') {
			out[j++] = '\\';
			out[j++] = '"';
		} else if (c == '\\') {
			out[j++] = '\\';
			out[j++] = '\\';
		} else {
			out[j++] = (char)c;
		}
	}
	out[j] = '\0';
	return out;
}

static int build_messages_json(struct arena *arena,
				const char *system_prompt,
				const char **messages, int n,
				char **out_json)
{
	size_t cap = 8192;
	size_t len = 0;
	char *buf = arena_alloc(arena, cap);
	if (!buf)
		return -ENOMEM;

	len += snprintf(buf + len, cap - len, "[");
	int first = 1;

	if (system_prompt && *system_prompt) {
		char *esc = escape_json_string(arena, system_prompt);
		if (!esc) { return -ENOMEM; }
		len += snprintf(buf + len, cap - len,
				"{\"role\":\"system\",\"content\":\"%s\"}", esc);
		first = 0;
	}

	for (int i = 0; i < n; i++) {
		if (!first)
			len += snprintf(buf + len, cap - len, ",");
		const char *role = (i % 2 == 0) ? "user" : "assistant";
		char *esc = escape_json_string(arena, messages[i]);
		if (!esc) { return -ENOMEM; }
		size_t needed = len + strlen(esc) + 64;
		if (needed > cap) {
			cap = needed * 2;
			char *new_buf = arena_alloc(arena, cap);
			if (!new_buf) { return -ENOMEM; }
			memcpy(new_buf, buf, len);
			buf = new_buf;
		}
		len += snprintf(buf + len, cap - len,
				"{\"role\":\"%s\",\"content\":\"%s\"}",
				role, esc);
		first = 0;
	}
	len += snprintf(buf + len, cap - len, "]");
	*out_json = buf;
	return 0;
}

struct llm_stream_ctx {
	sse_callback user_cb;
	void *user_data;
	struct arena *arena;
	char *accumulated;
	size_t acc_len;
	size_t acc_cap;
	struct tool_call *tool_calls;
	int tool_call_count;
	int tool_call_cap;
};

static void llm_stream_init(struct llm_stream_ctx *ctx, struct arena *arena,
			     sse_callback cb, void *user_data)
{
	ctx->user_cb = cb;
	ctx->user_data = user_data;
	ctx->arena = arena;
	ctx->acc_cap = 8192;
	ctx->accumulated = arena_alloc(arena, ctx->acc_cap);
	ctx->acc_len = 0;
	if (ctx->accumulated)
		ctx->accumulated[0] = '\0';
	ctx->tool_calls = NULL;
	ctx->tool_call_count = 0;
	ctx->tool_call_cap = 0;
}

static void llm_stream_append(struct llm_stream_ctx *ctx, const char *text)
{
	size_t tlen = strlen(text);
	if (ctx->acc_len + tlen + 1 > ctx->acc_cap) {
		size_t new_cap = (ctx->acc_len + tlen + 1) * 2;
		char *new_acc = arena_alloc(ctx->arena, new_cap);
		if (!new_acc)
			return;
		memcpy(new_acc, ctx->accumulated, ctx->acc_len);
		ctx->accumulated = new_acc;
		ctx->acc_cap = new_cap;
	}
	memcpy(ctx->accumulated + ctx->acc_len, text, tlen);
	ctx->acc_len += tlen;
	ctx->accumulated[ctx->acc_len] = '\0';
}

static struct tool_call *llm_stream_ensure_tool_call(struct llm_stream_ctx *ctx,
						      int index)
{
	if (index >= ctx->tool_call_cap) {
		int new_cap = index + 4;
		struct tool_call *new_tc = arena_alloc(ctx->arena, (size_t)new_cap * sizeof(*new_tc));
		if (!new_tc)
			return NULL;
		if (ctx->tool_calls)
			memcpy(new_tc, ctx->tool_calls, (size_t)ctx->tool_call_cap * sizeof(*new_tc));
		for (int i = ctx->tool_call_cap; i < new_cap; i++)
			memset(&new_tc[i], 0, sizeof(new_tc[i]));
		ctx->tool_calls = new_tc;
		ctx->tool_call_cap = new_cap;
	}
	if (index >= ctx->tool_call_count)
		ctx->tool_call_count = index + 1;
	return &ctx->tool_calls[index];
}

static void llm_stream_free(struct llm_stream_ctx *ctx)
{
	(void)ctx;
}

static void llm_stream_transfer_tool_calls(struct llm_stream_ctx *ctx,
					    struct chat_response *resp)
{
	if (ctx->tool_call_count <= 0) {
		resp->tool_calls = NULL;
		resp->tool_call_count = 0;
		return;
	}
	resp->tool_calls = arena_alloc(ctx->arena, (size_t)ctx->tool_call_count * sizeof(*resp->tool_calls));
	if (!resp->tool_calls) {
		resp->tool_call_count = 0;
		return;
	}
	resp->tool_call_count = ctx->tool_call_count;
	for (int i = 0; i < ctx->tool_call_count; i++) {
		strncpy(resp->tool_calls[i].id, ctx->tool_calls[i].id,
			sizeof(resp->tool_calls[i].id) - 1);
		strncpy(resp->tool_calls[i].name, ctx->tool_calls[i].name,
			sizeof(resp->tool_calls[i].name) - 1);
		resp->tool_calls[i].arguments = ctx->tool_calls[i].arguments;
		ctx->tool_calls[i].arguments = NULL;
	}
}

static int llm_sse_event_cb(const char *event, const char *data, void *ud)
{
	struct llm_stream_ctx *ctx = ud;
	(void)event;
	if (!data || !*data)
		return 0;

	if (strcmp(data, "[DONE]") == 0) {
		log_dbg("llm_sse_event_cb: [DONE]");
		return 0;
	}

	cJSON *root = cJSON_Parse(data);
	if (!root)
		return 0;

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

	cJSON *delta = cJSON_GetObjectItem(first, "delta");
	if (!delta) {
		cJSON *text_item = cJSON_GetObjectItem(first, "text");
		if (cJSON_IsString(text_item) && text_item->valuestring) {
			llm_stream_append(ctx, text_item->valuestring);
			if (ctx->user_cb)
				ctx->user_cb(text_item->valuestring, ctx->user_data);
		}
		cJSON_Delete(root);
		return 0;
	}

	cJSON *content = cJSON_GetObjectItem(delta, "content");
	if (cJSON_IsString(content) && content->valuestring) {
		llm_stream_append(ctx, content->valuestring);
		if (ctx->user_cb)
			ctx->user_cb(content->valuestring, ctx->user_data);
	}

	cJSON *tool_calls_arr = cJSON_GetObjectItem(delta, "tool_calls");
	if (cJSON_IsArray(tool_calls_arr)) {
		cJSON *tc_item;
		cJSON_ArrayForEach(tc_item, tool_calls_arr) {
			cJSON *idx_item = cJSON_GetObjectItem(tc_item, "index");
			int index = cJSON_IsNumber(idx_item) ? (int)idx_item->valuedouble : 0;

			struct tool_call *tc = llm_stream_ensure_tool_call(ctx, index);
			if (!tc)
				continue;

			cJSON *id_item = cJSON_GetObjectItem(tc_item, "id");
			if (cJSON_IsString(id_item) && id_item->valuestring) {
				strncpy(tc->id, id_item->valuestring,
					sizeof(tc->id) - 1);
			}

			cJSON *func_obj = cJSON_GetObjectItem(tc_item, "function");
			if (func_obj) {
				cJSON *name_item = cJSON_GetObjectItem(func_obj, "name");
				if (cJSON_IsString(name_item) && name_item->valuestring) {
					strncpy(tc->name, name_item->valuestring,
						sizeof(tc->name) - 1);
				}
				cJSON *args_item = cJSON_GetObjectItem(func_obj, "arguments");
				if (cJSON_IsString(args_item) && args_item->valuestring) {
					size_t existing = tc->arguments ? strlen(tc->arguments) : 0;
					size_t add_len = strlen(args_item->valuestring);
					char *new_args = arena_alloc(ctx->arena, existing + add_len + 1);
					if (new_args) {
						if (tc->arguments)
							memcpy(new_args, tc->arguments, existing);
						tc->arguments = new_args;
						memcpy(tc->arguments + existing,
						       args_item->valuestring,
						       add_len);
						tc->arguments[existing + add_len] = '\0';
					}
				}
			}
		}
	}

	cJSON_Delete(root);
	return 0;
}

static int llm_sse_http_cb(const char *data, size_t len, void *ud)
{
	struct sse_parser *parser = ud;
	sse_parser_feed(parser, data, len);
	return 0;
}

static int llm_chat(struct model *self, struct arena *arena,
		    const char *system_prompt,
		    const char **messages, int n,
		    sse_callback cb, void *user_data)
{
	if (!self || !self->api_key[0]) {
		log_err("llm_chat: no API key configured");
		return -EINVAL;
	}

	log_dbg("llm_chat: start, model=%s, api_base=%s", self->model_id, self->api_base);

	char *msgs_json = NULL;
	int rc = build_messages_json(arena, system_prompt, messages, n, &msgs_json);
	if (rc < 0)
		return rc;

	size_t body_cap = strlen(msgs_json) + 512;
	char *body = arena_alloc(arena, body_cap);
	if (!body) {
		return -ENOMEM;
	}

	int body_len = snprintf(body, body_cap,
		"{\"model\":\"%s\","
		"\"messages\":%s,"
		"\"stream\":true,"
		"\"max_tokens\":%d}",
		self->model_id, msgs_json,
		self->max_tokens > 0 ? self->max_tokens : 4096);

	if (body_len < 0 || (size_t)body_len >= body_cap) {
		return -EIO;
	}

	struct llm_stream_ctx ctx;
	llm_stream_init(&ctx, arena, cb, user_data);

	struct sse_parser parser;
	sse_parser_init(&parser, llm_sse_event_cb, &ctx);

	char url[512];
	snprintf(url, sizeof(url), "%s/chat/completions", self->api_base);

	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", self->api_key);
	const char *extra_headers[] = { auth_header };

	long timeout = self->timeout_seconds > 0 ? self->timeout_seconds : 300L;
	log_dbg("llm_chat: sending SSE request to %s, timeout=%lds", url, timeout);
	int status = http_post_sse_ex_timeout(url, body, (size_t)body_len,
				       "application/json",
				       extra_headers, 1, timeout,
				       llm_sse_http_cb, &parser);
	log_dbg("llm_chat: SSE request done, status=%d", status);

	sse_parser_free(&parser);

	if (status < 0) {
		log_err("llm_chat: SSE request failed: %d", status);
		return status;
	}
	if (status >= 400) {
		log_err("llm_chat: API returned HTTP %d", status);
		return -EIO;
	}

	return status;
}

static cJSON *build_structured_messages_cjson(const char *system_prompt,
					       struct chat_message *messages,
					       int msg_count)
{
	cJSON *arr = cJSON_CreateArray();

	if (system_prompt && *system_prompt) {
		cJSON *sys_msg = cJSON_CreateObject();
		cJSON_AddStringToObject(sys_msg, "role", "system");
		cJSON_AddStringToObject(sys_msg, "content", system_prompt);
		cJSON_AddItemToArray(arr, sys_msg);
	}

	for (int i = 0; i < msg_count; i++) {
		struct chat_message *m = &messages[i];
		cJSON *msg = cJSON_CreateObject();
		cJSON_AddStringToObject(msg, "role", m->role);

		if (m->content && *m->content)
			cJSON_AddStringToObject(msg, "content", m->content);
		else if (strcmp(m->role, "assistant") == 0 && m->tool_calls)
			cJSON_AddNullToObject(msg, "content");
		else if (m->content)
			cJSON_AddStringToObject(msg, "content", m->content);
		else
			cJSON_AddStringToObject(msg, "content", "");

		if (strcmp(m->role, "assistant") == 0 && m->tool_calls && m->tool_call_count > 0) {
			cJSON *tc_arr = cJSON_CreateArray();
			for (int j = 0; j < m->tool_call_count; j++) {
				cJSON *tc_obj = cJSON_CreateObject();
				cJSON_AddStringToObject(tc_obj, "id", m->tool_calls[j].id);
				cJSON_AddStringToObject(tc_obj, "type", "function");
				cJSON *func = cJSON_CreateObject();
				cJSON_AddStringToObject(func, "name", m->tool_calls[j].name);
				cJSON_AddStringToObject(func, "arguments",
							m->tool_calls[j].arguments ?
							m->tool_calls[j].arguments : "");
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

static cJSON *build_tools_cjson(struct tool_desc *tools, int tool_count)
{
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < tool_count; i++) {
		cJSON *tool = cJSON_CreateObject();
		cJSON_AddStringToObject(tool, "type", "function");

		cJSON *func = cJSON_CreateObject();
		cJSON_AddStringToObject(func, "name", tools[i].name);
		cJSON_AddStringToObject(func, "description", tools[i].desc);

		if (tools[i].args_spec[0]) {
			cJSON *params = cJSON_Parse(tools[i].args_spec);
			if (params)
				cJSON_AddItemToObject(func, "parameters", params);
			else
				cJSON_AddItemToObject(func, "parameters",
						      cJSON_CreateObject());
		} else {
			cJSON_AddItemToObject(func, "parameters",
					      cJSON_CreateObject());
		}

		cJSON_AddItemToObject(tool, "function", func);
		cJSON_AddItemToArray(arr, tool);
	}
	return arr;
}

static int llm_chat_with_tools(struct model *self, struct arena *arena,
			       const char *system_prompt,
			       struct chat_message *messages, int msg_count,
			       struct tool_desc *tools, int tool_count,
			       struct chat_response *response,
			       sse_callback thought_cb, void *thought_ud)
{
	if (!self || !self->api_key[0]) {
		log_err("llm_chat_with_tools: no API key configured");
		return -EINVAL;
	}

	log_dbg("llm_chat_with_tools: start, model=%s, tools=%d, msgs=%d",
		self->model_id, tool_count, msg_count);

	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "model", self->model_id);

	cJSON *msgs_arr = build_structured_messages_cjson(system_prompt,
							   messages, msg_count);
	cJSON_AddItemToObject(root, "messages", msgs_arr);

	if (tools && tool_count > 0) {
		cJSON *tools_arr = build_tools_cjson(tools, tool_count);
		cJSON_AddItemToObject(root, "tools", tools_arr);
		cJSON_AddStringToObject(root, "tool_choice", "auto");
	}

	cJSON_AddBoolToObject(root, "stream", 1);
	cJSON_AddNumberToObject(root, "max_tokens",
				self->max_tokens > 0 ? self->max_tokens : 4096);

	char *body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	if (!body)
		return -ENOMEM;

	struct llm_stream_ctx ctx;
	llm_stream_init(&ctx, arena, thought_cb, thought_ud);

	struct sse_parser parser;
	sse_parser_init(&parser, llm_sse_event_cb, &ctx);

	char url[512];
	snprintf(url, sizeof(url), "%s/chat/completions", self->api_base);

	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", self->api_key);
	const char *extra_headers[] = { auth_header };

	long timeout = self->timeout_seconds > 0 ? self->timeout_seconds : 300L;
	log_dbg("llm_chat_with_tools: sending SSE request to %s", url);
	int status = http_post_sse_ex_timeout(url, body, strlen(body),
				       "application/json",
				       extra_headers, 1, timeout,
				       llm_sse_http_cb, &parser);
	log_dbg("llm_chat_with_tools: SSE request done, status=%d", status);

	sse_parser_free(&parser);
	free(body);

	if (status < 0) {
		log_err("llm_chat_with_tools: SSE request failed: %d", status);
		return status;
	}
	if (status >= 400) {
		log_err("llm_chat_with_tools: API returned HTTP %d", status);
		return -EIO;
	}

	if (ctx.accumulated && *ctx.accumulated)
		response->content = ctx.accumulated;
	else {
		response->content = NULL;
	}

	llm_stream_transfer_tool_calls(&ctx, response);

	return status;
}

static int llm_generate(struct model *self, const char *prompt,
			const char *out_path)
{
	if (!self || !prompt)
		return -EINVAL;

	struct arena *arena = arena_create(8192);
	if (!arena)
		return -ENOMEM;

	const char *messages[] = { prompt };
	char *msgs_json = NULL;
	int rc = build_messages_json(arena, NULL, messages, 1, &msgs_json);
	if (rc < 0) {
		arena_destroy(arena);
		return rc;
	}

	size_t body_cap = strlen(msgs_json) + 256;
	char *body = malloc(body_cap);
	if (!body) {
		arena_destroy(arena);
		return -ENOMEM;
	}

	int body_len = snprintf(body, body_cap,
		"{\"model\":\"%s\","
		"\"messages\":%s,"
		"\"max_tokens\":%d}",
		self->model_id, msgs_json,
		self->max_tokens > 0 ? self->max_tokens : 4096);
	arena_destroy(arena);

	if (body_len < 0 || (size_t)body_len >= body_cap) {
		free(body);
		return -EIO;
	}

	struct http_response resp = {0};
	char url[512];
	snprintf(url, sizeof(url), "%s/chat/completions", self->api_base);
	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", self->api_key);
	const char *extra_headers[] = { auth_header };
	rc = http_post_ex(url, body, (size_t)body_len,
			  "application/json", extra_headers, 1, &resp);
	free(body);

	if (rc < 0) {
		http_response_free(&resp);
		return rc;
	}

	if (resp.status_code != 200) {
		log_err("llm_generate: API returned %d", resp.status_code);
		http_response_free(&resp);
		return -EIO;
	}

	rc = 0;
	if (out_path && resp.body)
		rc = file_write_all(out_path, resp.body, resp.body_len);

	http_response_free(&resp);
	return rc;
}

static void llm_destroy(struct model *self)
{
	if (self)
		free(self);
}

struct model *model_llm_create(const char *provider, const char *model_id,
			      const char *api_base, const char *api_key)
{
	struct model *m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	strncpy(m->provider, provider ? provider : "openai", sizeof(m->provider) - 1);
	strncpy(m->model_id, model_id ? model_id : "gpt-4o", sizeof(m->model_id) - 1);
	if (api_base)
		strncpy(m->api_base, api_base, sizeof(m->api_base) - 1);
	else
		strncpy(m->api_base, "https://api.openai.com/v1", sizeof(m->api_base) - 1);
	if (api_key)
		strncpy(m->api_key, api_key, sizeof(m->api_key) - 1);
	m->context_limit = 128000;
	m->max_tokens = 4096;
	m->timeout_seconds = 0;
	m->chat = llm_chat;
	m->chat_with_tools = llm_chat_with_tools;
	m->generate = llm_generate;
	m->destroy = llm_destroy;
	return m;
}

void model_destroy(struct model *m)
{
	if (m && m->destroy)
		m->destroy(m);
}

void chat_response_free(struct chat_response *resp)
{
	if (!resp)
		return;
	free(resp->content);
	resp->content = NULL;
	for (int i = 0; i < resp->tool_call_count; i++)
		free(resp->tool_calls[i].arguments);
	free(resp->tool_calls);
	resp->tool_calls = NULL;
	resp->tool_call_count = 0;
}

void chat_message_cleanup(struct chat_message *msg)
{
	if (!msg)
		return;
	free(msg->role);
	free(msg->content);
	free(msg->tool_call_id);
	for (int i = 0; i < msg->tool_call_count; i++)
		free(msg->tool_calls[i].arguments);
	free(msg->tool_calls);
	memset(msg, 0, sizeof(*msg));
}

void tool_call_cleanup(struct tool_call *tc)
{
	if (!tc)
		return;
	free(tc->arguments);
	tc->arguments = NULL;
}
