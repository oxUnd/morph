#ifndef MORPH_CLI_MODELS_SUBAGENTS_H
#define MORPH_CLI_MODELS_SUBAGENTS_H

#include "event/event.h"
#include "nb/vec.h"
#include "nb/cleanup.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include <stdint.h>

struct cli_transcript_model;
enum cli_subagents_segment_kind {
	CLI_SUBAGENTS_USER,
	CLI_SUBAGENTS_REASONING,
	CLI_SUBAGENTS_ASSISTANT,
	CLI_SUBAGENTS_TOOL,
	CLI_SUBAGENTS_PLAN,
};
struct cli_subagents_segment {
	enum cli_subagents_segment_kind kind;
	size_t tool_index;
	nb_buf text;
};
struct cli_subagents_thread {
	char id[256];
	struct cli_transcript_model *transcript;
	nb_vec segments;
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
