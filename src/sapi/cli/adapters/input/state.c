#include "sapi/cli/adapters/input/state.h"
#include "sapi/cli/adapters/input/callbacks.h"
#include "sapi/cli/application.h"
#include "util/error.h"

#include <stdlib.h>

int cli_input_init(struct cli_application *ctx)
{
	struct cli_input_state *input;
	int error;

	if (!ctx)
		return -EINVAL;
	if (ctx->input)
		return 0;
	input = calloc(1, sizeof(*input));
	if (!input)
		return -ENOMEM;
	error = pthread_mutex_init(&input->prompt_lock, NULL);
	if (error) {
		free(input);
		return -error;
	}
	input->prompt_lock_ready = 1;
	ctx->input = input;
	return 0;
}

void cli_input_cleanup(struct cli_application *ctx)
{
	if (!ctx || !ctx->input)
		return;
	cli_callbacks_cleanup(ctx);
	pthread_mutex_destroy(&ctx->input->prompt_lock);
	free(ctx->input->completion_sessions);
	free(ctx->input->readline_ready_input);
	free(ctx->input->footer_text);
	free(ctx->input);
	ctx->input = NULL;
}
