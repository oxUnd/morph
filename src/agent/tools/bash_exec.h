#ifndef BASH_EXEC_H
#define BASH_EXEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"

int bash_exec_init(struct tool_registry *reg);
void bash_exec_set_default_timeout(int seconds);
void bash_exec_clear_allowlist(void);
int bash_exec_allow_command(const char *command);
int bash_exec_allow_cwd(const char *cwd);

#ifdef __cplusplus
}
#endif

#endif
