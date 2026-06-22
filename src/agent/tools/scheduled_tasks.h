#ifndef SCHEDULED_TASKS_TOOL_H
#define SCHEDULED_TASKS_TOOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"
#include "db/database.h"
#include "db/scheduled_task.h"

int scheduled_tasks_tool_init(struct tool_registry *reg, struct db *db);
int scheduled_tasks_tool_init_events(
	struct tool_registry *reg, struct db *db,
	const struct scheduled_task_event_sink *events);
int scheduled_tasks_tool_set_time_anchor(struct tool_registry *reg,
					 int64_t time_anchor);

#ifdef __cplusplus
}
#endif

#endif
