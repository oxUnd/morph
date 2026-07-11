#ifndef CLI_COMMAND_REGISTRY_H
#define CLI_COMMAND_REGISTRY_H

#include "cli/internal.h"

struct cli_command {
	const char *name;
	int (*handler)(struct cli_context *ctx, int argc, char **argv);
	const char *desc;
	const char *usage;
};

int cli_command_register_many(const struct cli_command *entries, int count);
void cli_command_registry_clear(void);
const struct cli_command *cli_command_find(const char *name);
int cli_command_dispatch(struct cli_context *ctx, const char *input);
void cli_print_help(void);
char *cli_command_completion_generator(const char *text, int state);
int cli_commands_init(void);

int cli_register_runtime_commands(void);
int cli_register_session_commands(void);
int cli_register_memory_commands(void);
int cli_register_task_commands(void);
int cli_register_media_commands(void);
int cli_register_ext_commands(void);
int cli_register_skill_commands(void);
int cli_register_mcp_commands(void);
int cli_register_sync_commands(void);

#endif
