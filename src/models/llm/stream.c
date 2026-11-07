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

void llm_stream_init(struct llm_stream_ctx *ctx, struct arena *arena, sse_callback cb,
		     void *user_data)
{
	ctx->logger = NULL;
	ctx->user_cb = cb;
	ctx->stream_cb = NULL;
	ctx->user_data = user_data;
	ctx->arena = arena;
	if (morph_buf_init_arena(&ctx->accumulated, arena, 8192) != 0)
		memset(&ctx->accumulated, 0, sizeof(ctx->accumulated));
	if (morph_buf_init_arena(&ctx->reasoning, arena, 8192) != 0)
		memset(&ctx->reasoning, 0, sizeof(ctx->reasoning));
	ctx->tool_calls = NULL;
	ctx->tool_call_count = 0;
	ctx->tool_call_cap = 0;
	ctx->emitted = 0;
	memset(&ctx->usage, 0, sizeof(ctx->usage));
}

void llm_stream_init_typed(struct llm_stream_ctx *ctx, struct arena *arena, llm_stream_callback cb,
			   void *user_data)
{
	llm_stream_init(ctx, arena, NULL, user_data);
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

struct tool_call *llm_stream_ensure_tool_call(struct llm_stream_ctx *ctx, int index)
{
	if (!ctx || index < 0 || index > INT_MAX - 4)
		return NULL;
	if (index >= ctx->tool_call_cap) {
		int new_cap = index + 4;
		struct tool_call *new_tc =
			arena_alloc(ctx->arena, (size_t)new_cap * sizeof(*new_tc));
		if (!new_tc)
			return NULL;
		if (ctx->tool_calls)
			memcpy(new_tc, ctx->tool_calls,
			       (size_t)ctx->tool_call_cap * sizeof(*new_tc));
		for (int i = ctx->tool_call_cap; i < new_cap; i++)
			memset(&new_tc[i], 0, sizeof(new_tc[i]));
		ctx->tool_calls = new_tc;
		ctx->tool_call_cap = new_cap;
	}
	if (index >= ctx->tool_call_count)
		ctx->tool_call_count = index + 1;
	return &ctx->tool_calls[index];
}

void llm_stream_transfer_tool_calls(struct llm_stream_ctx *ctx, struct chat_response *resp)
{
	if (ctx->tool_call_count <= 0) {
		resp->tool_calls = NULL;
		resp->tool_call_count = 0;
		return;
	}
	resp->tool_calls =
		arena_alloc(ctx->arena, (size_t)ctx->tool_call_count * sizeof(*resp->tool_calls));
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
		resp->tool_calls[i].input_kind = ctx->tool_calls[i].input_kind;
	}
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

static int64_t json_i64(const struct json *object, const char *name)
{
	int64_t value = 0;

	(void)json_read_int(json_member_ci(object, name), &value);
	return value;
}

static void llm_usage_parse_details(struct model_usage *usage, const struct json *usage_obj)
{
	const struct json *prompt_details;
	const struct json *input_details;
	const struct json *completion_details;
	int64_t cached_tokens;
	int64_t candidate;

	if (!usage || !usage_obj)
		return;
	prompt_details = json_member_ci(usage_obj, "prompt_tokens_details");
	input_details = json_member_ci(usage_obj, "input_tokens_details");
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

	completion_details = json_member_ci(usage_obj, "completion_tokens_details");
	if (!completion_details)
		completion_details = json_member_ci(usage_obj, "output_tokens_details");
	usage->reasoning_tokens += json_i64(completion_details, "reasoning_tokens");
	usage->audio_tokens += json_i64(completion_details, "audio_tokens");
}

void llm_stream_parse_metadata(struct llm_stream_ctx *ctx, const struct json *root)
{
	const char *text;

	if (!ctx || !root)
		return;
	text = json_string_value(json_member_ci(root, "id"));
	if (text && !ctx->usage.response_id[0])
		llm_usage_copy(ctx->usage.response_id, sizeof(ctx->usage.response_id), text);
	text = json_string_value(json_member_ci(root, "model"));
	if (text && !ctx->usage.model[0])
		llm_usage_copy(ctx->usage.model, sizeof(ctx->usage.model), text);
	text = json_string_value(json_member_ci(root, "system_fingerprint"));
	if (text && !ctx->usage.system_fingerprint[0])
		llm_usage_copy(ctx->usage.system_fingerprint, sizeof(ctx->usage.system_fingerprint),
			       text);
	if (ctx->usage.created == 0)
		ctx->usage.created = json_i64(root, "created");
}

void llm_stream_parse_usage(struct llm_stream_ctx *ctx, const struct json *root)
{
	const struct json *usage_obj;

	if (!ctx || !root)
		return;
	usage_obj = json_member_ci(root, "usage");
	if (!json_is_object_value(usage_obj))
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

void llm_stream_parse_finish_reason(struct llm_stream_ctx *ctx, const struct json *choice)
{
	const char *finish;

	if (!ctx || !choice)
		return;
	finish = json_string_value(json_member_ci(choice, "finish_reason"));
	if (finish)
		llm_usage_copy(ctx->usage.finish_reason, sizeof(ctx->usage.finish_reason), finish);
}
