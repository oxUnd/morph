#ifndef MORPH_RUNTIME_TASK_RUNNER_H
#define MORPH_RUNTIME_TASK_RUNNER_H

#include "db/scheduled_task.h"

int runtime_task_run_agent(const struct scheduled_task *task,
	struct scheduled_task_action_result *result, void *user_data);

#endif
