#ifndef MORPH_RUNTIME_MCP_SAMPLING_H
#define MORPH_RUNTIME_MCP_SAMPLING_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

int runtime_mcp_sampling(const char *server, const char *method, const cJSON *params,
			 const void *context, cJSON **result, void *user_data);
int runtime_mcp_roots(const char *server, const char *method, const cJSON *params,
		      const void *context, cJSON **result, void *user_data);

#ifdef __cplusplus
}
#endif

#endif
