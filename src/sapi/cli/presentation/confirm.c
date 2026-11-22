#include "sapi/cli/presentation/confirm.h"

#include "sapi/cli/application.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"

#include <stdio.h>

void cli_confirm_prompt_show(struct cli_application *ctx, enum cli_confirm_prompt_kind kind,
			     const char *subject)
{
	const char *answer_hint = cli_chat_active(ctx) ? "" : " [y/N]: ";

	switch (kind) {
	case CLI_CONFIRM_PROMPT_REPLACE_DATABASE:
		cli_output_printf(ctx,
				  ANSI_BOLD ANSI_YELLOW
				  "? Replace %s with this backup? Current data will be backed "
				  "up.%s" ANSI_RESET,
				  subject ? subject : "", answer_hint);
		break;
	case CLI_CONFIRM_PROMPT_CLEAR_PROJECT_PERMISSIONS:
		cli_output_printf(
			ctx,
			ANSI_BOLD ANSI_YELLOW
			"? Revoke all persistent permissions for this project?%s" ANSI_RESET,
			answer_hint);
		break;
	case CLI_CONFIRM_PROMPT_CLEAR_ALL_PERMISSIONS:
		cli_output_printf(ctx,
				  ANSI_BOLD ANSI_RED
				  "? Type \"clear all\" to revoke permissions for every "
				  "project: " ANSI_RESET);
		break;
	}

	fflush(stdout);
}

void cli_confirm_prompt_end(struct cli_application *ctx)
{
	cli_output_putchar(ctx, '\n');
}
