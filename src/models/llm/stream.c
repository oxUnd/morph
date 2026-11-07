#include "stream.h"
#include "util/log.h"
#include "util/error.h"
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
	strncpy(dst, src, dst_cap - 1);
	dst[dst_cap - 1] = '\0';
}

void llm_stream_cleanup(struct llm_stream_ctx *ctx)
{
	if (!ctx)
		return;
	morph_buf_cleanup(&ctx->accumulated);
	morph_buf_cleanup(&ctx->reasoning);
	for (int i = 0; i < ctx->tool_call_count; i++)
		tool_call_cleanup(&ctx->tool_calls[i]);
	free(ctx->tool_calls);
	memset(ctx, 0, sizeof(*ctx));
}

void llm_stream_init(struct llm_stream_ctx *ctx, sse_callback cb, void *user_data)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->user_cb = cb;
	ctx->user_data = user_data;
	int rc = morph_buf_init(&ctx->accumulated, 8192);
	if (rc != 0)
		ctx->accumulated.failed = rc;
	rc = morph_buf_init(&ctx->reasoning, 8192);
	if (rc != 0)
		ctx->reasoning.failed = rc;
}

void llm_stream_init_typed(struct llm_stream_ctx *ctx, llm_stream_callback cb, void *user_data)
{
	llm_stream_init(ctx, NULL, user_data);
	ctx->stream_cb = cb;
}

void llm_stream_append(struct llm_stream_ctx *ctx, const char *text)
{
	(void)morph_buf_puts(&ctx->accumulated, text);
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

struct tool_call *llm_stream_tool_call_ensure_borrow(struct llm_stream_ctx *ctx, int index)
{
	if (index < 0 || index > INT_MAX - 4)
		return NULL;
	if (index >= ctx->tool_call_cap) {
		int new_cap = index + 4;
		if ((size_t)new_cap > SIZE_MAX / sizeof(*ctx->tool_calls))
			return NULL;
		struct tool_call *next =
			realloc(ctx->tool_calls, (size_t)new_cap * sizeof(*ctx->tool_calls));
		if (!next)
			return NULL;
		for (int i = ctx->tool_call_cap; i < new_cap; i++)
			memset(&next[i], 0, sizeof(next[i]));
		ctx->tool_calls = next;
		ctx->tool_call_cap = new_cap;
	}
	if (index >= ctx->tool_call_count)
		ctx->tool_call_count = index + 1;
	return &ctx->tool_calls[index];
}

void llm_stream_tool_calls_move(struct llm_stream_ctx *ctx, struct chat_response *resp)
{
	if (!ctx || !resp)
		return;
	for (int i = 0; resp->tool_calls && i < resp->tool_call_count; i++)
		tool_call_cleanup(&resp->tool_calls[i]);
	free(resp->tool_calls);
	resp->tool_calls = ctx->tool_calls;
	resp->tool_call_count = ctx->tool_call_count;
	ctx->tool_calls = NULL;
	ctx->tool_call_count = 0;
	ctx->tool_call_cap = 0;
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
		log_dbg(ctx ? ctx->logger : NULL,
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
	if (cJSON_IsString(finish) && finish->valuestring)
		llm_usage_copy(ctx->usage.finish_reason, sizeof(ctx->usage.finish_reason),
			       finish->valuestring);
}
