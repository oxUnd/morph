#ifndef MORPH_CLI_MODELS_TRANSCRIPT_H
#define MORPH_CLI_MODELS_TRANSCRIPT_H

#include "nb/vec.h"
#include "nb/cleanup.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>
#include "event/event.h"
#include <stdint.h>

struct cJSON;
struct model_history_item;

struct cli_transcript_tool {
	char *id;
	char *name;
	char *agent_id;
	char *args;
	char *subject;
	char *result;
	char *error;
	char *summary;
	char *patch_preview;
	nb_buf stream;
	struct morph_terminal_sanitizer sanitizer;
	int state;
	int timed_out;
	int historical;
	int interrupted;
	int shell;
	int process_poll;
	int process_running;
	double started;
	double ended;
};

struct cli_transcript_reasoning {
	size_t after_tools;
	nb_buf text;
};

struct cli_transcript_model {
	nb_vec tools;
	nb_vec reasoning;
	uint64_t revision;
	int finished;
	int allocation_failed;
};

/* out must be NULL; destroy releases the model and its owned contents. */
int cli_transcript_model_create(struct cli_transcript_model **out);
void cli_transcript_model_destroy(struct cli_transcript_model *model);

enum cli_transcript_change_kind {
	CLI_TRANSCRIPT_UNCHANGED,
	CLI_TRANSCRIPT_OBSERVATION,
	CLI_TRANSCRIPT_TOOL_STARTED,
	CLI_TRANSCRIPT_TOOL_RUNNING,
	CLI_TRANSCRIPT_TOOL_STREAMED,
	CLI_TRANSCRIPT_TOOL_FINISHED,
};

/* An index remains valid until reset; no pointer is retained across array growth. */
struct cli_transcript_change {
	enum cli_transcript_change_kind kind;
	size_t tool_index;
	size_t stream_offset;
	int observation_added;
};

int cli_transcript_model_reduce(struct cli_transcript_model *model, const struct morph_event *event,
				double now, struct cli_transcript_change *change);
int cli_transcript_model_reasoning_append(struct cli_transcript_model *model, const char *text,
					  size_t length, int begin);
int cli_transcript_model_restore(struct cli_transcript_model *model, const struct cJSON *call,
				 const struct model_history_item *result, size_t *tool_index);
int cli_transcript_model_set_preview(struct cli_transcript_model *model, size_t tool_index,
				     const char *preview);
void cli_transcript_model_finish(struct cli_transcript_model *model, double now);
const struct cli_transcript_tool *
cli_transcript_model_active_borrow(const struct cli_transcript_model *model);
int cli_transcript_tool_is_running(const struct cli_transcript_tool *tool);
int cli_transcript_tool_is_pending(const struct cli_transcript_tool *tool);

#endif
