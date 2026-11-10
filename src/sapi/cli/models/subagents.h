#ifndef MORPH_CLI_MODELS_SUBAGENTS_H
#define MORPH_CLI_MODELS_SUBAGENTS_H

#include "event/event.h"
#include "util/array.h"
#include "util/buf.h"
#include <stdint.h>

struct cli_transcript_model;
enum cli_subagents_segment_kind {
	CLI_SUBAGENTS_USER,
	CLI_SUBAGENTS_REASONING,
	CLI_SUBAGENTS_ASSISTANT,
	CLI_SUBAGENTS_TOOL,
};
struct cli_subagents_segment {
	enum cli_subagents_segment_kind kind;
	size_t tool_index;
	morph_buf_t text;
};
struct cli_subagents_thread {
	char id[256];
	struct cli_transcript_model *transcript;
	morph_array_t segments;
	int has_content;
	int64_t sequence;
	int reasoning_open;
	int content_open;
	int content_seen;
};

int cli_subagents_thread_create(const char *id, struct cli_subagents_thread **out);
void cli_subagents_thread_destroy(struct cli_subagents_thread *thread);
int cli_subagents_thread_event(struct cli_subagents_thread *thread,
			       const struct morph_event *event);
int cli_subagents_thread_replay(struct cli_subagents_thread *thread, const char *json);

#endif
