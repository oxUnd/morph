#include "runtime/runtime_internal.h"
#include "nb/err.h"

#include "agent/tools/scheduled_tasks.h"
#include "runtime/task_worker.h"
#include "nb/cleanup.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int runtime_tasks_execute(struct runtime *runtime, const char *args_json,
			  int64_t now, char **out_json)
{
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {0};
	char *json NB_CLEANUP(nb_charp_cleanup) = NULL;
	int rc;

	if (!runtime || !out_json)
		return NB_ERR_INVAL;
	if (*out_json)
		return NB_ERR_EXISTS;
	(void)scheduled_tasks_tool_set_time_anchor(&runtime->context.tools, now);
	(void)scheduled_tasks_tool_set_source_session(&runtime->context.tools,
		runtime->context.current_session.id);
	tool_result_init(&result);
	rc = tool_exec(&runtime->context.tools, "tasks",
		       args_json ? args_json : "{}", &result);
	if (rc != 0)
		return rc;
	json = strdup(result.text.ptr ? result.text.ptr : "{}");
	if (!json)
		return NB_ERR_NOMEM;
	*out_json = nb_charp_steal(&json);
	return 0;
}

int runtime_task_scheduler_start(
	struct runtime *runtime,
	scheduled_task_runner_fn runner, void *runner_user_data,
	runtime_task_notification_fn notification_cb, void *notification_user_data,
	int limit)
{
	if (!runtime)
		return NB_ERR_INVAL;
	return runtime_task_worker_start(&runtime->task_worker,
		&runtime->context.database, runner, runner_user_data,
		notification_cb, notification_user_data, limit);
}

void runtime_task_scheduler_stop(struct runtime *runtime)
{
	if (runtime)
		runtime_task_worker_stop(&runtime->task_worker);
}

int runtime_task_scheduler_running(const struct runtime *runtime)
{
	return runtime ? runtime->task_worker.started : 0;
}

int runtime_tasks_run_due_for_runtime(
	struct runtime *runtime, int limit, scheduled_task_runner_fn runner,
	void *runner_user_data, runtime_task_notification_fn notification_cb,
	void *notification_user_data)
{
	if (!runtime)
		return NB_ERR_INVAL;
	return runtime_task_run_due_collect(&runtime->context.database, limit,
		runner, runner_user_data, notification_cb, notification_user_data);
}
