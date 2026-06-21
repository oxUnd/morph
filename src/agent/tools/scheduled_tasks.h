#ifndef SCHEDULED_TASKS_TOOL_H
#define SCHEDULED_TASKS_TOOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"
#include "db/database.h"

int scheduled_tasks_tool_init(struct tool_registry *reg, struct db *db);

#ifdef __cplusplus
}
#endif

#endif
