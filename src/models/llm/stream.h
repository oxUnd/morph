#ifndef MORPH_LLM_STREAM_H
#define MORPH_LLM_STREAM_H

#include "models/llm.h"
#include "nb/buf.h"
#include "nb/vec.h"
#include "nb/cleanup.h"
#include "nb/cleanup.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

struct llm_stream_ctx {
	struct nb_log *logger;
	sse_callback user_cb;
	llm_stream_callback stream_cb;
	void *user_data;
	nb_buf accumulated;
	nb_buf reasoning;
	struct tool_call *tool_calls;
	int tool_call_count;
	nb_vec call_storage;
	nb_vec input_storage;
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

/* Stream arguments borrow native buffer storage until response publication. */
int llm_stream_call_input_append(struct llm_stream_ctx *ctx, struct tool_call *call,
				 const char *delta);
int llm_stream_call_input_set(struct llm_stream_ctx *ctx, struct tool_call *call,
			      const char *input);

/* Replace response call ownership, preserving its other fields, and clear ctx calls. */
int llm_stream_tool_calls_move(struct llm_stream_ctx *ctx, struct chat_response *resp);

void llm_usage_finalize(struct model *self, struct llm_stream_ctx *ctx, size_t request_bytes);

void llm_usage_report(struct model *self, struct llm_stream_ctx *ctx, size_t request_bytes);

void llm_stream_parse_metadata(struct llm_stream_ctx *ctx, cJSON *root);

void llm_stream_parse_usage(struct llm_stream_ctx *ctx, cJSON *root);

void llm_stream_parse_finish_reason(struct llm_stream_ctx *ctx, cJSON *choice);

#ifdef __cplusplus
}
#endif

#endif
