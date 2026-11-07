#ifndef MORPH_CLI_SERVICES_CONFIGURATION_MCP_H
#define MORPH_CLI_SERVICES_CONFIGURATION_MCP_H

#include "util/buf.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *cli_setup_mcp_summary(int role);
/* out must be NULL; success returns independent text, released with free(). */
int cli_setup_mcp_apply_copy(const char *text, int role, char **out);
int cli_setup_mcp_notes(int role, morph_buf_t *output);

#ifdef __cplusplus
}
#endif
#endif
