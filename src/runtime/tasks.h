#ifndef MORPH_RUNTIME_TASKS_H
#define MORPH_RUNTIME_TASKS_H

#include "agent/tools/scheduled_tasks.h"

#include <stdint.h>

struct runtime;

typedef void (*runtime_task_notification_fn)(
	const struct notification *notification, void *user_data);

char *runtime_scheduled_task_prompt(const struct scheduled_task *task);
char *runtime_scheduled_task_turn_id(const struct scheduled_task *task);
char *runtime_scheduled_task_display_prompt(const struct scheduled_task *task,
					    const char *task_prompt);

int runtime_tasks_execute(struct runtime *runtime, const char *args_json,
			  int64_t now, char **out_json);
int runtime_task_scheduler_start(
	struct runtime *runtime,
	scheduled_task_runner_fn runner, void *runner_user_data,
	runtime_task_notification_fn notification_cb, void *notification_user_data,
	int limit);
void runtime_task_scheduler_stop(struct runtime *runtime);
int runtime_task_scheduler_running(const struct runtime *runtime);
int runtime_tasks_run_due_for_runtime(
	struct runtime *runtime, int limit, scheduled_task_runner_fn runner,
	void *runner_user_data, runtime_task_notification_fn notification_cb,
	void *notification_user_data);

#endif
