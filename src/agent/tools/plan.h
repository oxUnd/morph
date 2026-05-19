#ifndef TOOL_PLAN_H
#define TOOL_PLAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"
#include "agent/plan.h"

struct model;

int plan_tool_init(struct tool_registry *reg, struct plan_registry *plans,
		   struct model *llm);

#ifdef __cplusplus
}
#endif

#endif
