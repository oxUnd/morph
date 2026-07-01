#ifndef RUNTIME_QUERY_TOOL_H
#define RUNTIME_QUERY_TOOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"

int runtime_query_tools_init(struct tool_registry *reg);

#ifdef __cplusplus
}
#endif

#endif
