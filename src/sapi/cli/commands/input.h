#ifndef MORPH_CLI_COMMANDS_INPUT_H
#define MORPH_CLI_COMMANDS_INPUT_H

struct cli_application;

/* Classify one submitted line: media path, slash command, or model turn. */
int cli_command_input_execute(struct cli_application *ctx, const char *input);

#endif
