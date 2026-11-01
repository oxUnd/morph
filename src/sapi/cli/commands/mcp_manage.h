#ifndef MORPH_CLI_COMMANDS_MCP_MANAGE_H
#define MORPH_CLI_COMMANDS_MCP_MANAGE_H
struct morph_log;
int cli_mcp_manage_run(struct morph_log *logger, int argc, char **argv, const char *config_path);
#endif
