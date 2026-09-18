#ifndef MORPH_EXEC_TOOL_H
#define MORPH_EXEC_TOOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"

struct tool_context;
struct config_exec;

int exec_tool_init(struct tool_registry *reg, struct tool_context *tctx,
		   const struct config_exec *config);

#ifdef __cplusplus
}
#endif

#endif
