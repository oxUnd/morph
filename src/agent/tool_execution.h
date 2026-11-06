#ifndef MORPH_TOOL_EXECUTION_H
#define MORPH_TOOL_EXECUTION_H

#include "agent/tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Clears an initialized caller-owned result; cleanup is required on all paths.
 * Returns zero only for business success. Arguments are borrowed until return. */
int tool_exec(struct tool_registry *reg, const char *name, const char *args_json,
	      struct tool_result *result);

#ifdef __cplusplus
}
#endif

#endif
