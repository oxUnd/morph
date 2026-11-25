#include "sapi/cli/services/scheduler.h"
#include <nb/err.h>
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include <nb/cleanup.h>
#include "persistence/orm/scheduled_task.h"
#include "agent/turn.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/application_internal.h"

static void cli_scheduler_task_notification(const struct notification *notification,
					    void *user_data)
{
	struct cli_application *ctx = user_data;
	if (!notification)
		return;

	if (ctx && cli_event_queue_post_notification(ctx, notification) != 0)
		nb_log_write(ctx ? ctx->logger : NULL, NB_LOG_WARN,
			     "failed to queue task notification");
}

void cli_scheduler_process_due(struct cli_application *ctx)
{
	int rc;
	if (!ctx || runtime_task_scheduler_running(ctx->runtime))
		return;

	rc = runtime_tasks_run_due_for_runtime(ctx->runtime, 50, NULL, NULL,
					       cli_scheduler_task_notification, ctx);
	if (rc < 0)
		nb_log_write(ctx ? ctx->logger : NULL, NB_LOG_WARN,
			     "failed to process due tasks: %s",
			     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
}

int cli_scheduler_start(struct cli_application *ctx)
{
	if (!ctx)
		return NB_ERR_INVAL;

	return runtime_task_scheduler_start(ctx->runtime, NULL, NULL,
					    cli_scheduler_task_notification, ctx, 50);
}
