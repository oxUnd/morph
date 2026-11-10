#ifndef MORPH_MULTI_AGENTS_H
#define MORPH_MULTI_AGENTS_H

#include "agent/agent_control.h"

#ifdef __cplusplus
extern "C" {
#endif
int multi_agents_register(struct tool_registry *tools, struct agent_control *control);
#ifdef __cplusplus
}
#endif
#endif
