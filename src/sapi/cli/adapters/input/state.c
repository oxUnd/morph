#include "sapi/cli/adapters/input/state.h"
#include "sapi/cli/adapters/input/callbacks.h"
#include "sapi/cli/application.h"
#include "errors.h"

#include <stdlib.h>

int cli_input_init(struct cli_application *ctx)
{
	struct cli_input_state *input;
	int error;

	if (!ctx)
		return NB_ERR_INVAL;
	if (ctx->input)
		return 0;
	input = calloc(1, sizeof(*input));
	if (!input)
		return NB_ERR_NOMEM;
	error = ((input->prompt_lock = nb_mutex_new(0)) ? NB_OK : NB_ERR_NOMEM);
	if (error) {
		free(input);
		return error;
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
	(nb_mutex_free(ctx->input->prompt_lock), ctx->input->prompt_lock = NULL);
	free(ctx->input->completion_sessions);
	free(ctx->input->readline_ready_input);
	free(ctx->input->footer_text);
	free(ctx->input);
	ctx->input = NULL;
}
