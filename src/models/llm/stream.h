#ifndef MORPH_LLM_STREAM_H
#define MORPH_LLM_STREAM_H

#include "models/llm.h"
#include "util/buf.h"
#include "util/arena.h"
#include "util/json.h"

struct llm_stream_ctx {
	struct morph_log *logger;
	sse_callback user_cb;
	llm_stream_callback stream_cb;
	void *user_data;
	struct arena *arena;
	morph_buf_t accumulated;
	morph_buf_t reasoning;
	struct tool_call *tool_calls;
	int tool_call_count;
	int tool_call_cap;
	int emitted;
	struct model_usage usage;
};

void llm_usage_copy(char *dst, size_t dst_cap, const char *src);

void llm_stream_init(struct llm_stream_ctx *ctx, struct arena *arena, sse_callback cb,
		     void *user_data);

void llm_stream_init_typed(struct llm_stream_ctx *ctx, struct arena *arena, llm_stream_callback cb,
			   void *user_data);

void llm_stream_append(struct llm_stream_ctx *ctx, const char *text);

int llm_stream_emit(struct llm_stream_ctx *ctx, enum llm_stream_kind kind, const char *text);

struct tool_call *llm_stream_ensure_tool_call(struct llm_stream_ctx *ctx, int index);

void llm_stream_transfer_tool_calls(struct llm_stream_ctx *ctx, struct chat_response *resp);

void llm_usage_finalize(struct model *self, struct llm_stream_ctx *ctx, size_t request_bytes);

void llm_usage_report(struct model *self, struct llm_stream_ctx *ctx, size_t request_bytes);

void llm_stream_parse_metadata(struct llm_stream_ctx *ctx, const struct json *root);

void llm_stream_parse_usage(struct llm_stream_ctx *ctx, const struct json *root);

void llm_stream_parse_finish_reason(struct llm_stream_ctx *ctx, const struct json *choice);

#endif
