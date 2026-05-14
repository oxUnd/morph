#ifndef SKILL_ACTIVATE_H
#define SKILL_ACTIVATE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"
#include "skill/skill.h"

int skill_activate_init(struct tool_registry *reg, struct skill_registry *skills);

#ifdef __cplusplus
}
#endif

#endif
