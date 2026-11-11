#ifndef MORPH_CLI_ADAPTERS_INPUT_STATE_H
#define MORPH_CLI_ADAPTERS_INPUT_STATE_H

#include <pthread.h>
#include <stdint.h>

struct cli_application;
struct cli_callback;
struct session;
struct morph_cancel_token;

/* Input adapters own callbacks, editable state and prompt constraints. */
struct cli_input_state {
	pthread_mutex_t prompt_lock;
	int prompt_lock_ready;
	struct cli_callback *callbacks;
	int callback_error;
	int64_t input_deadline_ms;
	int input_timed_out;
	const struct morph_cancel_token *input_cancel_token;
	char *readline_ready_input;
	int display_columns;
	int display_rows;
	char *footer_text;
	struct session *completion_sessions;
	int completion_count;
	int completion_index;
	int completion_length;
};

int cli_input_init(struct cli_application *ctx);
void cli_input_cleanup(struct cli_application *ctx);

#endif
