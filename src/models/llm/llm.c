#include "llm.h"
#include "agent/tool.h"
#include "util/log.h"
#include "util/file.h"
#include "util/utf8.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/image_util.h"
#include "http/client.h"
#include "http/sse.h"
#include "cJSON.h"
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static model_usage_callback g_usage_cb;
static __thread void *g_usage_user_data;

void model_set_usage_callback(model_usage_callback cb)
{
	g_usage_cb = cb;
}

void model_set_usage_user_data(void *user_data)
{
	g_usage_user_data = user_data;
}

void *model_get_usage_user_data(void)
{
	return g_usage_user_data;
}

void model_report_usage(const struct model_usage *usage)
{
	if (g_usage_cb && usage)
		g_usage_cb(usage, g_usage_user_data);
}

static int64_t estimate_tokens_from_bytes(size_t bytes)
{
	if (bytes == 0)
		return 0;
	return (int64_t)((bytes + 3) / 4);
}

static void usage_strncpy(char *dst, size_t dst_cap, const char *src)
{
	if (!dst || dst_cap == 0 || !src)
		return;
	strncpy(dst, src, dst_cap - 1);
	dst[dst_cap - 1] = '\0';
}

static void model_clear_last_error(struct model *self)
{
	if (self)
		self->last_error[0] = '\0';
}

static void model_set_last_error(struct model *self, const char *fmt, ...)
{
	va_list ap;

	if (!self || !fmt)
		return;
	va_start(ap, fmt);
	vsnprintf(self->last_error, sizeof(self->last_error), fmt, ap);
	va_end(ap);
}

static char *json_print_arena(struct arena *arena, cJSON *root)
{
	char *heap;
	char *out;

	if (!arena || !root)
		return NULL;
	heap = cJSON_PrintUnformatted(root);
	if (!heap)
		return NULL;
	out = arena_strdup(arena, heap);
	free(heap);
	return out;
}

static cJSON *build_simple_messages_cjson(const char *system_prompt,
					  const char **messages, int n)
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
		cJSON_AddStringToObject(msg, "content",
					messages && messages[i] ?
					messages[i] : "");
		cJSON_AddItemToArray(arr, msg);
	}
	return arr;
}

static int build_chat_body_json(struct arena *arena, const char *model_id,
				const char *system_prompt,
				const char **messages, int n,
				int max_tokens, int stream,
				char **out_body, size_t *out_body_len)
{
	cJSON *root;
	cJSON *msgs;
	char *body;

	if (!arena || !model_id || !out_body || !out_body_len)
		return -EINVAL;
	*out_body = NULL;
	*out_body_len = 0;
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
	cJSON_AddNumberToObject(root, "max_tokens",
				max_tokens > 0 ? max_tokens : 4096);
	body = json_print_arena(arena, root);
	cJSON_Delete(root);
	if (!body)
		return -ENOMEM;
	*out_body = body;
	*out_body_len = strlen(body);
	return 0;
}

struct llm_stream_ctx {
	sse_callback user_cb;
	llm_stream_callback stream_cb;
	void *user_data;
	struct arena *arena;
	morph_buf_t accumulated;
	struct tool_call *tool_calls;
	int tool_call_count;
	int tool_call_cap;
	struct model_usage usage;
};

static void llm_stream_init(struct llm_stream_ctx *ctx, struct arena *arena,
			     sse_callback cb, void *user_data)
{
	ctx->user_cb = cb;
	ctx->stream_cb = NULL;
	ctx->user_data = user_data;
	ctx->arena = arena;
	if (morph_buf_init_arena(&ctx->accumulated, arena, 8192) != 0)
		memset(&ctx->accumulated, 0, sizeof(ctx->accumulated));
	ctx->tool_calls = NULL;
	ctx->tool_call_count = 0;
	ctx->tool_call_cap = 0;
	memset(&ctx->usage, 0, sizeof(ctx->usage));
}

static void llm_stream_init_typed(struct llm_stream_ctx *ctx,
				   struct arena *arena,
				   llm_stream_callback cb, void *user_data)
{
	llm_stream_init(ctx, arena, NULL, user_data);
	ctx->stream_cb = cb;
}

static void llm_stream_append(struct llm_stream_ctx *ctx, const char *text)
{
	(void)morph_buf_puts(&ctx->accumulated, text);
}

static int llm_stream_emit(struct llm_stream_ctx *ctx,
			   enum llm_stream_kind kind, const char *text)
{
	if (!ctx || !text || !*text)
		return 0;
	if (ctx->stream_cb)
		return ctx->stream_cb(kind, text, ctx->user_data);
	if (kind == LLM_STREAM_CONTENT && ctx->user_cb)
		return ctx->user_cb(text, ctx->user_data);
	return 0;
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
		resp->tool_calls[i].arguments =
			ctx->tool_calls[i].arguments
			? arena_strdup(ctx->arena, ctx->tool_calls[i].arguments)
			: arena_strdup(ctx->arena, "");
	}
}

static void llm_usage_finalize(struct model *self, struct llm_stream_ctx *ctx,
			       size_t request_bytes)
{
	if (!self || !ctx)
		return;
	if (!ctx->usage.provider[0])
		usage_strncpy(ctx->usage.provider,
			      sizeof(ctx->usage.provider), self->provider);
	if (!ctx->usage.model[0])
		usage_strncpy(ctx->usage.model, sizeof(ctx->usage.model),
			      self->model_id);
	if (!ctx->usage.kind[0])
		usage_strncpy(ctx->usage.kind, sizeof(ctx->usage.kind),
			      "model_text");
	if (!ctx->usage.usage_source[0]) {
		ctx->usage.input_tokens = estimate_tokens_from_bytes(request_bytes);
		ctx->usage.output_tokens =
			estimate_tokens_from_bytes(ctx->accumulated.len);
		ctx->usage.total_tokens = ctx->usage.input_tokens +
			ctx->usage.output_tokens;
		usage_strncpy(ctx->usage.usage_source,
			      sizeof(ctx->usage.usage_source), "estimated");
	}
	if (ctx->usage.total_tokens == 0)
		ctx->usage.total_tokens = ctx->usage.input_tokens +
			ctx->usage.output_tokens;
}

static void llm_usage_report(struct model *self, struct llm_stream_ctx *ctx,
			     size_t request_bytes)
{
	if (!ctx)
		return;
	llm_usage_finalize(self, ctx, request_bytes);
	model_report_usage(&ctx->usage);
}

static int64_t json_i64(cJSON *obj, const char *name)
{
	cJSON *item;

	if (!obj || !name)
		return 0;
	item = cJSON_GetObjectItem(obj, name);
	if (!cJSON_IsNumber(item))
		return 0;
	return (int64_t)item->valuedouble;
}

static void llm_usage_parse_details(struct model_usage *usage,
				    cJSON *usage_obj)
{
	cJSON *prompt_details;
	cJSON *completion_details;

	if (!usage || !usage_obj)
		return;
	prompt_details = cJSON_GetObjectItem(usage_obj,
					     "prompt_tokens_details");
	if (!prompt_details)
		prompt_details = cJSON_GetObjectItem(usage_obj,
						     "input_tokens_details");
	usage->cached_tokens += json_i64(prompt_details, "cached_tokens");
	usage->audio_tokens += json_i64(prompt_details, "audio_tokens");
	usage->image_tokens += json_i64(prompt_details, "image_tokens");

	completion_details = cJSON_GetObjectItem(usage_obj,
						 "completion_tokens_details");
	if (!completion_details)
		completion_details = cJSON_GetObjectItem(usage_obj,
							 "output_tokens_details");
	usage->reasoning_tokens += json_i64(completion_details,
					    "reasoning_tokens");
	usage->audio_tokens += json_i64(completion_details, "audio_tokens");
}

static void llm_stream_parse_metadata(struct llm_stream_ctx *ctx, cJSON *root)
{
	cJSON *item;

	if (!ctx || !root)
		return;
	item = cJSON_GetObjectItem(root, "id");
	if (cJSON_IsString(item) && item->valuestring &&
	    !ctx->usage.response_id[0])
		usage_strncpy(ctx->usage.response_id,
			      sizeof(ctx->usage.response_id),
			      item->valuestring);
	item = cJSON_GetObjectItem(root, "model");
	if (cJSON_IsString(item) && item->valuestring && !ctx->usage.model[0])
		usage_strncpy(ctx->usage.model, sizeof(ctx->usage.model),
			      item->valuestring);
	item = cJSON_GetObjectItem(root, "system_fingerprint");
	if (cJSON_IsString(item) && item->valuestring &&
	    !ctx->usage.system_fingerprint[0])
		usage_strncpy(ctx->usage.system_fingerprint,
			      sizeof(ctx->usage.system_fingerprint),
			      item->valuestring);
	item = cJSON_GetObjectItem(root, "created");
	if (cJSON_IsNumber(item) && ctx->usage.created == 0)
		ctx->usage.created = (int64_t)item->valuedouble;
}

static void llm_stream_parse_usage(struct llm_stream_ctx *ctx, cJSON *root)
{
	cJSON *usage_obj;

	if (!ctx || !root)
		return;
	usage_obj = cJSON_GetObjectItem(root, "usage");
	if (!cJSON_IsObject(usage_obj))
		return;
	ctx->usage.input_tokens = json_i64(usage_obj, "prompt_tokens");
	if (ctx->usage.input_tokens == 0)
		ctx->usage.input_tokens = json_i64(usage_obj, "input_tokens");
	ctx->usage.output_tokens = json_i64(usage_obj, "completion_tokens");
	if (ctx->usage.output_tokens == 0)
		ctx->usage.output_tokens = json_i64(usage_obj, "output_tokens");
	ctx->usage.total_tokens = json_i64(usage_obj, "total_tokens");
	llm_usage_parse_details(&ctx->usage, usage_obj);
	usage_strncpy(ctx->usage.usage_source,
		      sizeof(ctx->usage.usage_source), "provider");
}

static void llm_stream_parse_finish_reason(struct llm_stream_ctx *ctx,
					   cJSON *choice)
{
	cJSON *finish;

	if (!ctx || !choice)
		return;
	finish = cJSON_GetObjectItem(choice, "finish_reason");
	if (cJSON_IsString(finish) && finish->valuestring)
		usage_strncpy(ctx->usage.finish_reason,
			      sizeof(ctx->usage.finish_reason),
			      finish->valuestring);
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
				int rc = ctx->user_cb(text_item->valuestring,
						      ctx->user_data);
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
		int rc = llm_stream_emit(ctx, LLM_STREAM_CONTENT,
					  content->valuestring);
		if (rc != 0) {
			cJSON_Delete(root);
			return rc;
		}
	}

	cJSON *reasoning = cJSON_GetObjectItem(delta, "reasoning_content");
	if (cJSON_IsString(reasoning) && reasoning->valuestring) {
		int rc = llm_stream_emit(ctx, LLM_STREAM_REASONING,
					  reasoning->valuestring);
		if (rc != 0) {
			cJSON_Delete(root);
			return rc;
		}
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

struct llm_http_ctx {
	struct sse_parser *parser;
	struct arena *arena;
	morph_buf_t error_buf;
};

static int llm_http_cb(const char *data, size_t len, void *ud)
{
	struct llm_http_ctx *hctx = ud;
	if (hctx->parser) {
		int rc = sse_parser_feed(hctx->parser, data, len);
		if (rc != 0)
			return rc;
	}
	(void)morph_buf_append(&hctx->error_buf, data, len);
	return 0;
}

static const char *llm_extract_error(const char *raw, struct arena *arena)
{
	if (!raw || !*raw)
		return NULL;
	cJSON *root = cJSON_Parse(raw);
	if (!root)
		return NULL;
	const char *msg = NULL;
	cJSON *err_obj = cJSON_GetObjectItem(root, "error");
	if (cJSON_IsObject(err_obj)) {
		cJSON *msg_item = cJSON_GetObjectItem(err_obj, "message");
		if (cJSON_IsString(msg_item) && msg_item->valuestring)
			msg = arena_strdup(arena, msg_item->valuestring);
	}
	cJSON_Delete(root);
	return msg;
}

static int llm_chat(struct model *self, struct arena *arena,
		    const char *system_prompt,
		    const char **messages, int n,
		    sse_callback cb, void *user_data)
{
	if (!self || !self->api_key[0]) {
		log_err("llm_chat: no API key configured");
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	}

	log_dbg("llm_chat: start, model=%s, api_base=%s", self->model_id, self->api_base);

	char *body = NULL;
	size_t body_len = 0;
	int rc = build_chat_body_json(arena, self->model_id, system_prompt,
				      messages, n, self->max_tokens, 1,
				      &body, &body_len);
	if (rc < 0)
		return rc;

	size_t clean_len = utf8_sanitize_into(body, body, body_len);
	body[clean_len] = '\0';
	body_len = clean_len;

	struct llm_stream_ctx ctx;
	llm_stream_init(&ctx, arena, cb, user_data);

	struct sse_parser parser;
	sse_parser_init(&parser, llm_sse_event_cb, &ctx);

	struct llm_http_ctx hctx;
	hctx.parser = &parser;
	hctx.arena = arena;
	if (morph_buf_init_arena(&hctx.error_buf, arena, 4096) != 0)
		memset(&hctx.error_buf, 0, sizeof(hctx.error_buf));

	char url[512];
	snprintf(url, sizeof(url), "%s/chat/completions", self->api_base);

	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", self->api_key);
	const char *extra_headers[] = { auth_header };

	long timeout = self->timeout_seconds > 0 ? self->timeout_seconds : 300L;
	log_dbg("llm_chat: sending SSE request to %s, timeout=%lds", url, timeout);
	int status = http_post_sse_ex_timeout(url, body, body_len,
				       "application/json",
				       extra_headers, 1, timeout,
				       llm_http_cb, &hctx);
	log_dbg("llm_chat: SSE request done, status=%d", status);

	sse_parser_free(&parser);

	if (status < 0) {
		log_err("llm_chat: SSE request failed: %d", status);
		return status;
	}
	if (status >= 400) {
		const char *detail = NULL;
		if (hctx.error_buf.len > 0)
			detail = llm_extract_error(hctx.error_buf.data, arena);
		if (detail)
			log_err("llm_chat: API returned HTTP %d: %s",
				status, detail);
		else
			log_err("llm_chat: API returned HTTP %d", status);
		MORPH_RETURN(MORPH_ERR_API);
	}

	llm_usage_report(self, &ctx, body_len);
	return status;
}

static int add_system_message(cJSON *messages, const char *system_prompt)
{
	if (!messages || !system_prompt || !*system_prompt)
		return 0;
	cJSON *msg = cJSON_CreateObject();
	if (!msg)
		return -ENOMEM;
	cJSON_AddStringToObject(msg, "role", "system");
	cJSON_AddStringToObject(msg, "content", system_prompt);
	cJSON_AddItemToArray(messages, msg);
	return 0;
}

static int llm_chat_with_image(struct model *self, struct arena *arena,
			       const char *system_prompt,
			       const char *prompt,
			       const char *image_path,
			       sse_callback cb, void *user_data)
{
	model_clear_last_error(self);
	if (!self || !self->api_key[0]) {
		log_err("llm_chat_with_image: no API key configured");
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	}
	if (!arena || !prompt || !*prompt || !image_path || !*image_path)
		return -EINVAL;

	char *b64 = image_encode_base64(image_path, 2048);
	if (!b64) {
		log_err("llm_chat_with_image: failed to encode image: %s",
			image_path);
		model_set_last_error(self, "failed to read or encode image: %s",
				     image_path);
		MORPH_RETURN(MORPH_ERR_FORMAT);
	}

	cJSON *root = cJSON_CreateObject();
	cJSON *messages = cJSON_CreateArray();
	if (!root || !messages) {
		cJSON_Delete(root);
		cJSON_Delete(messages);
		free(b64);
		return -ENOMEM;
	}
	cJSON_AddStringToObject(root, "model", self->model_id);
	cJSON_AddItemToObject(root, "messages", messages);
	if (add_system_message(messages, system_prompt) < 0) {
		cJSON_Delete(root);
		free(b64);
		return -ENOMEM;
	}

	cJSON *user = cJSON_CreateObject();
	cJSON *content = cJSON_CreateArray();
	cJSON *text = cJSON_CreateObject();
	cJSON *image = cJSON_CreateObject();
	cJSON *image_url = cJSON_CreateObject();
	if (!user || !content || !text || !image || !image_url) {
		cJSON_Delete(user);
		cJSON_Delete(content);
		cJSON_Delete(text);
		cJSON_Delete(image);
		cJSON_Delete(image_url);
		cJSON_Delete(root);
		free(b64);
		return -ENOMEM;
	}
	cJSON_AddStringToObject(user, "role", "user");
	cJSON_AddItemToObject(user, "content", content);
	cJSON_AddStringToObject(text, "type", "text");
	cJSON_AddStringToObject(text, "text", prompt);
	cJSON_AddItemToArray(content, text);

	size_t uri_len = strlen("data:image/png;base64,") + strlen(b64) + 1;
	char *data_uri = arena_alloc(arena, uri_len);
	if (!data_uri) {
		cJSON_Delete(root);
		free(b64);
		return -ENOMEM;
	}
	snprintf(data_uri, uri_len, "data:image/png;base64,%s", b64);
	free(b64);

	cJSON_AddStringToObject(image, "type", "image_url");
	cJSON_AddStringToObject(image_url, "url", data_uri);
	cJSON_AddItemToObject(image, "image_url", image_url);
	cJSON_AddItemToArray(content, image);
	cJSON_AddItemToArray(messages, user);
	cJSON_AddBoolToObject(root, "stream", 1);
	{
		cJSON *opts = cJSON_CreateObject();
		if (opts) {
			cJSON_AddBoolToObject(opts, "include_usage", 1);
			cJSON_AddItemToObject(root, "stream_options", opts);
		}
	}
	cJSON_AddNumberToObject(root, "max_tokens",
				self->max_tokens > 0 ? self->max_tokens : 4096);

	size_t body_cap = 8192 + uri_len + strlen(prompt);
	char *body = arena_alloc(arena, body_cap);
	while (body && !cJSON_PrintPreallocated(root, body,
						(int)body_cap, 0)) {
		body_cap *= 2;
		body = arena_alloc(arena, body_cap);
	}
	cJSON_Delete(root);
	if (!body)
		return -ENOMEM;

	size_t body_len = strlen(body);
	size_t clean_len = utf8_sanitize_into(body, body, body_len);
	body[clean_len] = '\0';
	body_len = clean_len;

	struct llm_stream_ctx ctx;
	llm_stream_init(&ctx, arena, cb, user_data);

	struct sse_parser parser;
	sse_parser_init(&parser, llm_sse_event_cb, &ctx);

	struct llm_http_ctx hctx;
	hctx.parser = &parser;
	hctx.arena = arena;
	if (morph_buf_init_arena(&hctx.error_buf, arena, 4096) != 0)
		memset(&hctx.error_buf, 0, sizeof(hctx.error_buf));

	char url[512];
	snprintf(url, sizeof(url), "%s/chat/completions", self->api_base);

	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s",
		 self->api_key);
	const char *extra_headers[] = { auth_header };

	long timeout = self->timeout_seconds > 0 ? self->timeout_seconds : 300L;
	log_dbg("llm_chat_with_image: sending SSE request to %s", url);
	int status = http_post_sse_ex_timeout(url, body, body_len,
					      "application/json",
					      extra_headers, 1, timeout,
					      llm_http_cb, &hctx);
	log_dbg("llm_chat_with_image: SSE request done, status=%d", status);

	sse_parser_free(&parser);

	if (status < 0) {
		log_err("llm_chat_with_image: SSE request failed: %d", status);
		model_set_last_error(self, "HTTP/SSE request failed: %s",
				     morph_strerror(status));
		return status;
	}
	if (status >= 400) {
		const char *detail = NULL;
		if (hctx.error_buf.len > 0)
			detail = llm_extract_error(hctx.error_buf.data, arena);
		if (detail) {
			log_err("llm_chat_with_image: API returned HTTP %d: %s",
				status, detail);
			model_set_last_error(self, "HTTP %d: %s", status, detail);
		} else {
			log_err("llm_chat_with_image: API returned HTTP %d",
				status);
			model_set_last_error(self, "HTTP %d from %s", status,
					     self->api_base);
		}
		MORPH_RETURN(MORPH_ERR_API);
	}

	llm_usage_report(self, &ctx, body_len);
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

static cJSON *normalize_params_to_schema(cJSON *params)
{
	if (!params)
		return NULL;
	if (cJSON_IsObject(params)) {
		if (!cJSON_HasObjectItem(params, "type"))
			cJSON_AddStringToObject(params, "type", "object");
		return params;
	}
	if (cJSON_IsArray(params)) {
		cJSON *schema = cJSON_CreateObject();
		cJSON_AddStringToObject(schema, "type", "object");
		cJSON *props = cJSON_CreateObject();
		cJSON *required = cJSON_CreateArray();
		cJSON *item;
		cJSON_ArrayForEach(item, params) {
			cJSON *name = cJSON_GetObjectItem(item, "name");
			if (!cJSON_IsString(name))
				continue;
			cJSON *prop = cJSON_CreateObject();
			cJSON_AddStringToObject(prop, "type", "string");
			cJSON *desc = cJSON_GetObjectItem(item, "description");
			if (cJSON_IsString(desc))
				cJSON_AddStringToObject(prop, "description",
							desc->valuestring);
			cJSON_AddItemToObject(props, name->valuestring, prop);
			cJSON *req = cJSON_GetObjectItem(item, "required");
			if (cJSON_IsBool(req) && cJSON_IsTrue(req))
				cJSON_AddItemToArray(required,
						     cJSON_CreateString(name->valuestring));
		}
		cJSON_AddItemToObject(schema, "properties", props);
		if (cJSON_GetArraySize(required) > 0)
			cJSON_AddItemToObject(schema, "required", required);
		else
			cJSON_Delete(required);
		cJSON_Delete(params);
		return schema;
	}
	cJSON_Delete(params);
	return NULL;
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

		cJSON *params = NULL;
		if (tools[i].args_spec[0])
			params = cJSON_Parse(tools[i].args_spec);
		params = normalize_params_to_schema(params);
		if (params)
			cJSON_AddItemToObject(func, "parameters", params);
		else
			cJSON_AddItemToObject(func, "parameters",
					      cJSON_Parse("{\"type\":\"object\",\"properties\":{}}"));

		cJSON_AddItemToObject(tool, "function", func);
		cJSON_AddItemToArray(arr, tool);
	}
	return arr;
}

static int llm_chat_with_tools_impl(struct model *self, struct arena *arena,
				    const char *system_prompt,
				    struct chat_message *messages,
				    int msg_count,
				    struct tool_desc *tools, int tool_count,
				    struct chat_response *response,
				    sse_callback thought_cb,
				    llm_stream_callback stream_cb,
				    void *stream_ud)
{
	if (!self || !self->api_key[0]) {
		log_err("llm_chat_with_tools: no API key configured");
		model_set_last_error(self, "no API key configured");
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	}
	if (!response)
		return -EINVAL;

	model_clear_last_error(self);
	memset(response, 0, sizeof(*response));
	response->arena = arena;

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
	{
		cJSON *opts = cJSON_CreateObject();
		if (opts) {
			cJSON_AddBoolToObject(opts, "include_usage", 1);
			cJSON_AddItemToObject(root, "stream_options", opts);
		}
	}
	cJSON_AddNumberToObject(root, "max_tokens",
				self->max_tokens > 0 ? self->max_tokens : 4096);

	size_t body_cap = 8192;
	char *body = arena_alloc(arena, body_cap);
	while (body && !cJSON_PrintPreallocated(root, body, (int)body_cap, 0)) {
		body_cap *= 2;
		body = arena_alloc(arena, body_cap);
	}
	cJSON_Delete(root);

	if (!body)
		return -ENOMEM;

	size_t body_len = strlen(body);
	size_t clean_len = utf8_sanitize_into(body, body, body_len);
	body[clean_len] = '\0';

	struct llm_stream_ctx ctx;
	if (stream_cb)
		llm_stream_init_typed(&ctx, arena, stream_cb, stream_ud);
	else
		llm_stream_init(&ctx, arena, thought_cb, stream_ud);

	struct sse_parser parser;
	sse_parser_init(&parser, llm_sse_event_cb, &ctx);

	struct llm_http_ctx hctx;
	hctx.parser = &parser;
	hctx.arena = arena;
	if (morph_buf_init_arena(&hctx.error_buf, arena, 4096) != 0)
		memset(&hctx.error_buf, 0, sizeof(hctx.error_buf));

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
				       llm_http_cb, &hctx);
	log_dbg("llm_chat_with_tools: SSE request done, status=%d", status);

	sse_parser_free(&parser);

	if (status < 0) {
		log_err("llm_chat_with_tools: SSE request failed: %d", status);
		model_set_last_error(self, "HTTP/SSE request failed: %s",
				     morph_strerror(status));
		return status;
	}
	if (status >= 400) {
		const char *detail = NULL;
		if (hctx.error_buf.len > 0)
			detail = llm_extract_error(hctx.error_buf.data, arena);
		if (detail) {
			log_err("llm_chat_with_tools: API returned HTTP %d: %s",
				status, detail);
			model_set_last_error(self, "HTTP %d: %s", status, detail);
			response->content = (char *)detail;
		} else {
			log_err("llm_chat_with_tools: API returned HTTP %d",
				status);
			model_set_last_error(self, "HTTP %d from %s", status, url);
		}
		MORPH_RETURN(MORPH_ERR_API);
	}

	if (ctx.accumulated.len > 0)
		response->content = ctx.accumulated.data;
	else {
		response->content = NULL;
	}

	llm_stream_transfer_tool_calls(&ctx, response);
	llm_usage_finalize(self, &ctx, strlen(body));
	response->usage = ctx.usage;
	model_report_usage(&response->usage);

	return status;
}

static int llm_chat_with_tools(struct model *self, struct arena *arena,
			       const char *system_prompt,
			       struct chat_message *messages, int msg_count,
			       struct tool_desc *tools, int tool_count,
			       struct chat_response *response,
			       sse_callback thought_cb, void *thought_ud)
{
	return llm_chat_with_tools_impl(self, arena, system_prompt, messages,
					msg_count, tools, tool_count,
					response, thought_cb, NULL, thought_ud);
}

static int llm_chat_with_tools_stream(struct model *self, struct arena *arena,
				      const char *system_prompt,
				      struct chat_message *messages,
				      int msg_count,
				      struct tool_desc *tools, int tool_count,
				      struct chat_response *response,
				      llm_stream_callback stream_cb,
				      void *stream_ud)
{
	return llm_chat_with_tools_impl(self, arena, system_prompt, messages,
					msg_count, tools, tool_count,
					response, NULL, stream_cb, stream_ud);
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
	char *body = NULL;
	size_t body_len = 0;
	int rc = build_chat_body_json(arena, self->model_id, NULL, messages, 1,
				      self->max_tokens, 0, &body, &body_len);
	if (rc < 0) {
		arena_destroy(arena);
		return rc;
	}

	struct http_response resp = {0};
	char url[512];
	snprintf(url, sizeof(url), "%s/chat/completions", self->api_base);
	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", self->api_key);
	const char *extra_headers[] = { auth_header };
	rc = http_post_ex(url, body, body_len,
			  "application/json", extra_headers, 1, &resp);
	arena_destroy(arena);

	if (rc < 0) {
		http_response_free(&resp);
		return rc;
	}

	if (resp.status_code != 200) {
		log_err("llm_generate: API returned %d", resp.status_code);
		http_response_free(&resp);
		MORPH_RETURN(MORPH_ERR_API);
	}

	rc = 0;
	if (out_path && resp.body.data)
		rc = file_write_all(out_path, resp.body.data, resp.body.len);

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
	m->chat_with_tools_stream = llm_chat_with_tools_stream;
	m->chat_with_image = llm_chat_with_image;
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
	if (resp->arena) {
		memset(resp, 0, sizeof(*resp));
		return;
	}
	free(resp->content);
	resp->content = NULL;
	for (int i = 0; i < resp->tool_call_count; i++)
		free(resp->tool_calls[i].arguments);
	free(resp->tool_calls);
	resp->tool_calls = NULL;
	resp->tool_call_count = 0;
}

void chat_message_cleanup(struct chat_message *msg, struct arena *arena)
{
	if (!msg)
		return;
	if (arena) {
		memset(msg, 0, sizeof(*msg));
		return;
	}
	free(msg->role);
	free(msg->content);
	free(msg->tool_call_id);
	for (int i = 0; i < msg->tool_call_count; i++)
		free(msg->tool_calls[i].arguments);
	free(msg->tool_calls);
	memset(msg, 0, sizeof(*msg));
}

void tool_call_cleanup(struct tool_call *tc, struct arena *arena)
{
	if (!tc)
		return;
	if (arena) {
		memset(tc, 0, sizeof(*tc));
		return;
	}
	free(tc->arguments);
	tc->arguments = NULL;
}
