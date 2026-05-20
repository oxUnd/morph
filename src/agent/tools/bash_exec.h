#ifndef BASH_EXEC_H
#define BASH_EXEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"

int bash_exec_init(struct tool_registry *reg);
void bash_exec_set_default_timeout(int seconds);

#ifdef __cplusplus
}
#endif

#endif
