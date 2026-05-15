#ifndef BASH_EXEC_H
#define BASH_EXEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"

int bash_exec_init(struct tool_registry *reg);

#ifdef __cplusplus
}
#endif

#endif
