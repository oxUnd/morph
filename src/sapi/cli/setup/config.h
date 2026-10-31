#ifndef MORPH_CLI_SETUP_CONFIG_H
#define MORPH_CLI_SETUP_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Merge setup-owned fields while retaining all other configuration values. */
int cli_setup_config_merge(const char *old, const char *patch, char **out);
/* Retain one nonempty credential source per model; direct keys take precedence. */
int cli_setup_config_credentials(const char *text, char **out);
/* Add one MCP server unless its name, endpoint or package is already configured. */
int cli_setup_config_mcp_add(const char *text, const char *server, char **out);
int cli_setup_config_role(const char *text);

#ifdef __cplusplus
}
#endif
#endif
