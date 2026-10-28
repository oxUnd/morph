#ifndef CLI_COMMAND_REGISTRY_H
#define CLI_COMMAND_REGISTRY_H

struct cli_context;
#include "sapi/cli/internal.h"

struct cli_command {
	const char *name;
	int (*handler)(struct cli_context *ctx, int argc, char **argv);
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
int cli_command_dispatch(struct cli_context *ctx, const char *input);
void cli_print_help(struct cli_context *ctx);
char *cli_command_completion_generator(const char *text, int state);

struct cli_command_group cli_share_commands(void);
struct cli_command_group cli_runtime_commands(void);
struct cli_command_group cli_session_commands(void);
struct cli_command_group cli_memory_commands(void);
struct cli_command_group cli_task_commands(void);
struct cli_command_group cli_media_commands(void);
struct cli_command_group cli_ext_commands(void);
struct cli_command_group cli_skill_commands(void);
struct cli_command_group cli_mcp_commands(void);
struct cli_command_group cli_sync_commands(void);
struct cli_command_group cli_permission_commands(void);
struct cli_command_group cli_hook_commands(void);

#endif
