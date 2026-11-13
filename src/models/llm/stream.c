#include "stream.h"
#include "nb/str.h"
#include "nb/utf8.h"
#include "nb/log.h"
#include "errors.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t estimate_tokens_from_bytes(size_t bytes)
{
	if (bytes == 0)
		return 0;
	return (int64_t)((bytes + 3) / 4);
}

void llm_usage_copy(char *dst, size_t dst_cap, const char *src)
{
	if (!dst || dst_cap == 0 || !src)
		return;
	(void)nb_str_copy_to(nb_str_from_parts(src, nb_utf8_clamp_bytes(src, dst_cap - 1)), dst,
			     (dst_cap - 1) + 1);
	dst[dst_cap - 1] = '\0';
}

static void llm_stream_calls_clear(struct llm_stream_ctx *ctx)
{
	for (size_t i = 0; i < ctx->call_storage.len; i++) {
		struct tool_call *call = nb_vec_at(&ctx->call_storage, i);
		nb_buf *input = nb_vec_at(&ctx->input_storage, i);
		if (input && call->arguments == input->ptr)
			call->arguments = NULL;
		tool_call_cleanup(call);
		nb_buf_destroy(input);
	}
	nb_vec_clear(&ctx->call_storage);
	nb_vec_clear(&ctx->input_storage);
	ctx->tool_calls = NULL;
	ctx->tool_call_count = 0;
}

void llm_stream_cleanup(struct llm_stream_ctx *ctx)
{
	if (!ctx)
		return;
	nb_buf_destroy(&ctx->accumulated);
	nb_buf_destroy(&ctx->reasoning);
	free(ctx->terminal_detail);
	llm_stream_calls_clear(ctx);
	nb_vec_destroy(&ctx->call_storage);
	nb_vec_destroy(&ctx->input_storage);
	memset(ctx, 0, sizeof(*ctx));
}

void llm_stream_init(struct llm_stream_ctx *ctx, sse_callback cb, void *user_data)
{
	memset(ctx, 0, sizeof(*ctx));
	nb_vec_init(&ctx->call_storage, sizeof(struct tool_call));
	nb_vec_init(&ctx->input_storage, sizeof(nb_buf));
	ctx->user_cb = cb;
	ctx->user_data = user_data;
	int rc = (nb_buf_init(&ctx->accumulated),
		  nb_buf_reserve(&ctx->accumulated, 8192) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		nb_buf_set_error(&ctx->accumulated, rc);
	rc = (nb_buf_init(&ctx->reasoning),
	      nb_buf_reserve(&ctx->reasoning, 8192) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		nb_buf_set_error(&ctx->reasoning, rc);
}

void llm_stream_init_typed(struct llm_stream_ctx *ctx, llm_stream_callback cb, void *user_data)
{
	llm_stream_init(ctx, NULL, user_data);
	ctx->stream_cb = cb;
}

void llm_stream_append(struct llm_stream_ctx *ctx, const char *text)
{
	(void)nb_buf_append_cstr(&ctx->accumulated, text);
}

int llm_stream_emit(struct llm_stream_ctx *ctx, enum llm_stream_kind kind, const char *text)
{
	if (!ctx || !text || !*text)
		return 0;
	ctx->emitted = 1;
	if (ctx->stream_cb)
		return ctx->stream_cb(kind, text, ctx->user_data);
	if (kind == LLM_STREAM_CONTENT && ctx->user_cb)
		return ctx->user_cb(text, ctx->user_data);
	return 0;
}

int llm_stream_string_copy(char **owner, const char *text)
{
	char *copy NB_CLEANUP(nb_charp_cleanup) = NULL;

	if (!owner || !text)
		return NB_ERR_INVAL;
	copy = strdup(text);
	if (!copy)
		return NB_ERR_NOMEM;
	free(*owner);
	*owner = nb_charp_steal(&copy);
	return 0;
}

struct tool_call *llm_stream_tool_call_ensure_borrow(struct llm_stream_ctx *ctx, int index)
{
	if (!ctx || index < 0 || index > INT_MAX - 4)
		return NULL;
	size_t count = (size_t)index + 1;
	if (!nb_vec_reserve(&ctx->call_storage, count))
		return NULL;
	ctx->tool_calls = (struct tool_call *)ctx->call_storage.data;
	if (!nb_vec_reserve(&ctx->input_storage, count))
		return NULL;
	if (count > ctx->call_storage.len) {
		size_t added = count - ctx->call_storage.len;
		void *calls = nb_vec_push_n(&ctx->call_storage, added);
		void *inputs = nb_vec_push_n(&ctx->input_storage, added);
		memset(calls, 0, added * ctx->call_storage.elem_size);
		memset(inputs, 0, added * ctx->input_storage.elem_size);
		ctx->tool_call_count = (int)count;
	}
	return nb_vec_at(&ctx->call_storage, (size_t)index);
}

static nb_buf *llm_stream_call_input_borrow(struct llm_stream_ctx *ctx, struct tool_call *call)
{
	if (!ctx || !call || !ctx->tool_calls)
		return NULL;
	ptrdiff_t index = call - ctx->tool_calls;
	if (index < 0 || index >= ctx->tool_call_count)
		return NULL;
	return nb_vec_at(&ctx->input_storage, (size_t)index);
}

int llm_stream_call_input_append(struct llm_stream_ctx *ctx, struct tool_call *call,
				 const char *delta)
{
	nb_buf *input = llm_stream_call_input_borrow(ctx, call);
	if (!input)
		return NB_ERR_PROTOCOL;
	char *external = call->arguments != input->ptr ? call->arguments : NULL;
	if (external && !nb_buf_append_cstr(input, external))
		return NB_ERR_NOMEM;
	if (!nb_buf_reserve(input, 0) || !nb_buf_append_cstr(input, delta ? delta : ""))
		return NB_ERR_NOMEM;
	free(external);
	call->arguments = input->ptr;
	return 0;
}

int llm_stream_call_input_set(struct llm_stream_ctx *ctx, struct tool_call *call, const char *text)
{
	nb_buf *input = llm_stream_call_input_borrow(ctx, call);
	nb_buf candidate NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!input)
		return NB_ERR_PROTOCOL;
	if (!nb_buf_reserve(&candidate, 0) || !nb_buf_append_cstr(&candidate, text ? text : ""))
		return NB_ERR_NOMEM;
	if (call->arguments && call->arguments != input->ptr)
		free(call->arguments);
	nb_buf_move(input, &candidate);
	call->arguments = input->ptr;
	return 0;
}

int llm_stream_tool_calls_move(struct llm_stream_ctx *ctx, struct chat_response *resp)
{
	if (!ctx || !resp)
		return NB_ERR_INVAL;
	struct chat_response candidate NB_CLEANUP(chat_response_free) = {0};
	if (ctx->tool_call_count) {
		candidate.tool_calls =
			calloc((size_t)ctx->tool_call_count, sizeof(*candidate.tool_calls));
		if (!candidate.tool_calls)
			return NB_ERR_NOMEM;
	}
	for (int i = 0; i < ctx->tool_call_count; i++) {
		candidate.tool_call_count = i + 1;
		int rc = tool_call_clone(&ctx->tool_calls[i], &candidate.tool_calls[i]);
		if (rc)
			return rc;
	}
	for (int i = 0; i < resp->tool_call_count; i++)
		tool_call_cleanup(&resp->tool_calls[i]);
	free(resp->tool_calls);
	resp->tool_calls = candidate.tool_calls;
	resp->tool_call_count = candidate.tool_call_count;
	candidate.tool_calls = NULL;
	candidate.tool_call_count = 0;
	llm_stream_calls_clear(ctx);
	return 0;
}

void llm_usage_finalize(struct model *self, struct llm_stream_ctx *ctx, size_t request_bytes)
{
	if (!self || !ctx)
		return;
	if (!ctx->usage.provider[0])
		llm_usage_copy(ctx->usage.provider, sizeof(ctx->usage.provider), self->provider);
	if (!ctx->usage.model[0])
		llm_usage_copy(ctx->usage.model, sizeof(ctx->usage.model), self->model_id);
	if (!ctx->usage.kind[0])
		llm_usage_copy(ctx->usage.kind, sizeof(ctx->usage.kind), "model_text");
	if (!ctx->usage.usage_source[0]) {
		ctx->usage.input_tokens = estimate_tokens_from_bytes(request_bytes);
		ctx->usage.output_tokens = estimate_tokens_from_bytes(ctx->accumulated.len);
		ctx->usage.total_tokens = ctx->usage.input_tokens + ctx->usage.output_tokens;
		llm_usage_copy(ctx->usage.usage_source, sizeof(ctx->usage.usage_source),
			       "estimated");
	}
	if (ctx->usage.total_tokens == 0)
		ctx->usage.total_tokens = ctx->usage.input_tokens + ctx->usage.output_tokens;
}

void llm_usage_report(struct model *self, struct llm_stream_ctx *ctx, size_t request_bytes)
{
	if (!ctx)
		return;
	llm_usage_finalize(self, ctx, request_bytes);
	model_report_usage(self, &ctx->usage);
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

static void llm_usage_parse_details(struct model_usage *usage, cJSON *usage_obj)
{
	cJSON *prompt_details;
	cJSON *input_details;
	cJSON *completion_details;
	int64_t cached_tokens;
	int64_t candidate;

	if (!usage || !usage_obj)
		return;
	prompt_details = cJSON_GetObjectItem(usage_obj, "prompt_tokens_details");
	input_details = cJSON_GetObjectItem(usage_obj, "input_tokens_details");
	cached_tokens = json_i64(usage_obj, "cached_tokens");
	candidate = json_i64(usage_obj, "prompt_cache_hit_tokens");
	if (candidate > cached_tokens)
		cached_tokens = candidate;
	candidate = json_i64(prompt_details, "cached_tokens");
	if (candidate > cached_tokens)
		cached_tokens = candidate;
	candidate = json_i64(input_details, "cached_tokens");
	if (candidate > cached_tokens)
		cached_tokens = candidate;
	if (cached_tokens > usage->cached_tokens)
		usage->cached_tokens = cached_tokens;
	if (!prompt_details)
		prompt_details = input_details;
	usage->audio_tokens += json_i64(prompt_details, "audio_tokens");
	usage->image_tokens += json_i64(prompt_details, "image_tokens");

	completion_details = cJSON_GetObjectItem(usage_obj, "completion_tokens_details");
	if (!completion_details)
		completion_details = cJSON_GetObjectItem(usage_obj, "output_tokens_details");
	usage->reasoning_tokens += json_i64(completion_details, "reasoning_tokens");
	usage->audio_tokens += json_i64(completion_details, "audio_tokens");
}

void llm_stream_parse_metadata(struct llm_stream_ctx *ctx, cJSON *root)
{
	cJSON *item;

	if (!ctx || !root)
		return;
	item = cJSON_GetObjectItem(root, "id");
	if (cJSON_IsString(item) && item->valuestring && !ctx->usage.response_id[0])
		llm_usage_copy(ctx->usage.response_id, sizeof(ctx->usage.response_id),
			       item->valuestring);
	item = cJSON_GetObjectItem(root, "model");
	if (cJSON_IsString(item) && item->valuestring && !ctx->usage.model[0])
		llm_usage_copy(ctx->usage.model, sizeof(ctx->usage.model), item->valuestring);
	item = cJSON_GetObjectItem(root, "system_fingerprint");
	if (cJSON_IsString(item) && item->valuestring && !ctx->usage.system_fingerprint[0])
		llm_usage_copy(ctx->usage.system_fingerprint, sizeof(ctx->usage.system_fingerprint),
			       item->valuestring);
	item = cJSON_GetObjectItem(root, "created");
	if (cJSON_IsNumber(item) && ctx->usage.created == 0)
		ctx->usage.created = (int64_t)item->valuedouble;
}

void llm_stream_parse_usage(struct llm_stream_ctx *ctx, cJSON *root)
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
	llm_usage_copy(ctx->usage.usage_source, sizeof(ctx->usage.usage_source), "provider");
	if (ctx->usage.input_tokens > 0)
		nb_log_write(
			ctx ? ctx->logger : NULL, NB_LOG_DEBUG,
			"llm usage: prompt cache %lld/%lld input tokens (%.1f%% hit)",
			(long long)ctx->usage.cached_tokens, (long long)ctx->usage.input_tokens,
			(double)ctx->usage.cached_tokens * 100.0 / (double)ctx->usage.input_tokens);
}

void llm_stream_parse_finish_reason(struct llm_stream_ctx *ctx, cJSON *choice)
{
	cJSON *finish;

	if (!ctx || !choice)
		return;
	finish = cJSON_GetObjectItem(choice, "finish_reason");
	if (cJSON_IsString(finish) && finish->valuestring && finish->valuestring[0]) {
		llm_usage_copy(ctx->usage.finish_reason, sizeof(ctx->usage.finish_reason),
			       finish->valuestring);
		ctx->terminal_received = 1;
	}
}
