#ifndef BASH_EXEC_H
#define BASH_EXEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"

struct tool_context;

int bash_exec_init(struct tool_registry *reg, struct tool_context *tctx);
void bash_exec_set_default_timeout(int seconds);
void bash_exec_set_resource_limits(int max_memory_mb, int max_open_files);

#ifdef __cplusplus
}
#endif

#endif
