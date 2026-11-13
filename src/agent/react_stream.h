#ifndef MORPH_REACT_STREAM_H
#define MORPH_REACT_STREAM_H

#include "agent/react_output.h"
#include "nb/thread.h"
#include "models/llm.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Text is borrowed only for the callback. Callbacks must not mutate the stream. */
typedef int (*react_stream_emit_fn)(enum react_step_type type, const char *text, void *user_data);

struct react_stream_options {
	react_stream_emit_fn emit;
	void *user_data;
	const nb_atomic_int *cancelled;
};

/* One model request owns this stream and its reusable text buffer. */
struct react_stream {
	struct react_stream_options options;
	nb_buf text;
	nb_buf reasoning;
	size_t emitted;
	size_t scanned;
	size_t candidate;
	size_t prefix_start;
	size_t prefix_length;
	int candidate_possible;
	int in_final;
	int skip_separator;
	int typed_content;
	int emitting;
	int error;
};

int react_stream_init(struct react_stream *stream, const struct react_stream_options *options);
void react_stream_cleanup(struct react_stream *stream);
int react_stream_finish(struct react_stream *stream);
int react_stream_cb(const char *token, void *user_data);
int react_typed_stream_cb(enum llm_stream_kind kind, const char *token, void *user_data);
/* Shares legacy final-marker recognition with final-response normalization. */
const char *react_stream_final_borrow(const char *base, const char *from, size_t *marker_len);

#ifdef __cplusplus
}
#endif

#endif
