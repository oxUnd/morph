#ifndef MORPH_LLM_STREAM_H
#define MORPH_LLM_STREAM_H

#include "models/llm.h"
#include "util/buf.h"
#include "util/cleanup.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

struct llm_stream_ctx {
	struct morph_log *logger;
	sse_callback user_cb;
	llm_stream_callback stream_cb;
	void *user_data;
	morph_buf_t accumulated;
	morph_buf_t reasoning;
	struct tool_call *tool_calls;
	int tool_call_count;
	int tool_call_cap;
	int emitted;
	int terminal_received;
	int terminal_error;
	char *terminal_detail;
	struct model_usage usage;
};

void llm_stream_cleanup(struct llm_stream_ctx *ctx);

void llm_usage_copy(char *dst, size_t dst_cap, const char *src);

void llm_stream_init(struct llm_stream_ctx *ctx, sse_callback cb, void *user_data);

void llm_stream_init_typed(struct llm_stream_ctx *ctx, llm_stream_callback cb, void *user_data);

void llm_stream_append(struct llm_stream_ctx *ctx, const char *text);

int llm_stream_emit(struct llm_stream_ctx *ctx, enum llm_stream_kind kind, const char *text);

int llm_stream_string_copy(char **owner, const char *text);

/* Borrowed slot: another ensure may grow the array and invalidate this pointer.
 * Call arguments remain owned by ctx until call ownership is moved out. */
struct tool_call *llm_stream_tool_call_ensure_borrow(struct llm_stream_ctx *ctx, int index);

/* Replace response call ownership, preserving its other fields, and clear ctx calls. */
void llm_stream_tool_calls_move(struct llm_stream_ctx *ctx, struct chat_response *resp);

void llm_usage_finalize(struct model *self, struct llm_stream_ctx *ctx, size_t request_bytes);

void llm_usage_report(struct model *self, struct llm_stream_ctx *ctx, size_t request_bytes);

void llm_stream_parse_metadata(struct llm_stream_ctx *ctx, cJSON *root);

void llm_stream_parse_usage(struct llm_stream_ctx *ctx, cJSON *root);

void llm_stream_parse_finish_reason(struct llm_stream_ctx *ctx, cJSON *choice);

#ifdef __cplusplus
}
#endif

#endif
