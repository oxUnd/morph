#include "sapi/cli/commands/input.h"

#include "sapi/cli/application.h"
#include "sapi/cli/commands/media.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/services/scheduler.h"
#include "sapi/cli/services/turn.h"
#include <errno.h>

int cli_command_input_execute(struct cli_application *ctx, const char *input)
{
	int handled = 0;
	int rc;

	if (!ctx || !input)
		return -EINVAL;
	cli_turn_prepare(ctx);
	rc = cli_media_handle_path(ctx, input, &handled);
	if (handled)
		return rc;
	if (input[0] == '/') {
		rc = cli_command_dispatch(ctx, input);
		cli_scheduler_process_due(ctx);
		return rc;
	}
	return cli_turn_execute_prompt(ctx, input);
}
