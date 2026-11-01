#ifndef MORPH_CLI_MODELS_PRESENTATION_H
#define MORPH_CLI_MODELS_PRESENTATION_H
#include "util/buf.h"
#include "db/scheduled_task.h"
#include <stdint.h>

enum cli_presentation_mode {
	CLI_PRESENT_INTERACTIVE,
	CLI_PRESENT_ONCE_PLAIN,
	CLI_PRESENT_EVENTS_JSON,
};

struct cli_input_status {
	int enabled;
	int padding_rows;
	int distance;
	int footer_rows;
	int composer_rows;
	int suspended;
	int64_t updated;
	int tokens;
	int limit;
	int64_t credits;
	int session_titles_pending;
	struct scheduled_task_counts tasks;
};

struct cli_output {
	int color_disabled;
	int structured;
	int command_result_pending;
	/* Logical history tail, including deferred scrollback, capped at two. */
	int trailing_newlines;
	morph_buf_t *capture;
	int capture_styled;
	/* Captured history advances the tail; alternate-screen frames do not. */
	int capture_history;
};

#endif
