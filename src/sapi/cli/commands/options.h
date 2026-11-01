#ifndef MORPH_CLI_COMMANDS_OPTIONS_H
#define MORPH_CLI_COMMANDS_OPTIONS_H

#include "sapi/cli/models/presentation.h"

struct cli_options {
	const char *config_path;
	const char *workdir;
	const char *one_shot_prompt;
	const char *session_name;
	int trace_json;
	int show_version;
	int show_help;
	int no_color;
	int events_json;
	int command_index;
};

int cli_options_parse(int argc, char **argv, struct cli_options *options);
enum cli_presentation_mode cli_options_presentation_mode(const struct cli_options *options);
int cli_options_print_usage(const struct cli_options *options);
int cli_options_emit_result(const char *command, const char *output);

#endif
