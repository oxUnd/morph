#ifndef MORPH_CLI_ADAPTERS_INPUT_STATE_H
#define MORPH_CLI_ADAPTERS_INPUT_STATE_H

#include <nb/thread.h>
#include <stdint.h>

struct cli_application;
struct cli_callback;
struct session;
struct nb_cancel;

/* Input adapters own callbacks, editable state and prompt constraints. */
struct cli_input_state {
	nb_mutex *prompt_lock;
	int prompt_lock_ready;
	struct cli_callback *callbacks;
	int callback_error;
	int64_t input_deadline_ms;
	int input_timed_out;
	const struct nb_cancel *input_cancel_token;
};

int cli_input_init(struct cli_application *ctx);
void cli_input_cleanup(struct cli_application *ctx);

#endif
