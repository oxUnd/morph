#ifndef MORPH_RUNTIME_MCP_ELICITATION_H
#define MORPH_RUNTIME_MCP_ELICITATION_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

int runtime_mcp_elicitation(const char *server, const char *method, const cJSON *params,
			    const void *context, cJSON **result, void *user_data);

#ifdef __cplusplus
}
#endif

#endif
