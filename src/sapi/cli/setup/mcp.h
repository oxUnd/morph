#ifndef MORPH_CLI_SETUP_MCP_H
#define MORPH_CLI_SETUP_MCP_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *cli_setup_mcp_summary(int role);
int cli_setup_mcp_apply(const char *text, int role, char **out);
void cli_setup_mcp_notes(int role, FILE *output);

#ifdef __cplusplus
}
#endif
#endif
