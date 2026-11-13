#ifndef MORPH_CLI_PRESENTATION_CONFIRM_H
#define MORPH_CLI_PRESENTATION_CONFIRM_H

struct cli_application;

enum cli_confirm_prompt_kind {
	CLI_CONFIRM_PROMPT_REPLACE_DATABASE,
	CLI_CONFIRM_PROMPT_CLEAR_PROJECT_PERMISSIONS,
	CLI_CONFIRM_PROMPT_CLEAR_ALL_PERMISSIONS,
};

/* Prints the question and flushes it before the blocking terminal read. */
void cli_confirm_prompt_show(struct cli_application *ctx, enum cli_confirm_prompt_kind kind,
			     const char *subject);

/* Ends a prompt that was never answered from a terminal. */
void cli_confirm_prompt_end(struct cli_application *ctx);

#endif
