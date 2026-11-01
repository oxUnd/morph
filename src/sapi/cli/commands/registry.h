#ifndef MORPH_CLI_COMMANDS_REGISTRY_H
#define MORPH_CLI_COMMANDS_REGISTRY_H

struct cli_application;

struct cli_command {
	const char *name;
	int (*handler)(struct cli_application *ctx, int argc, char **argv);
	const char *desc;
	const char *usage;
};

struct cli_command_group {
	const struct cli_command *entries;
	int count;
	const char *category;
};

const struct cli_command *cli_command_find(const char *name);
const struct cli_command *cli_command_find_input(const char *input);
const char *cli_cmd_arg(int argc, char **argv, int idx);
int cli_argv_split(const char *input, char **argv, int max_args);
int cli_command_dispatch(struct cli_application *ctx, const char *input);
void cli_print_help(struct cli_application *ctx);
char *cli_command_completion_generator(const char *text, int state);

#endif
