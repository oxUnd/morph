#include "sapi/cli/services/scheduler.h"
#include "db/scheduled_task.h"
#include "agent/turn.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/application_internal.h"

int cli_scheduler_run_task(const struct scheduled_task *task,
			   struct scheduled_task_action_result *result, void *user_data)
{
	struct cli_application *ctx = user_data;
	struct runtime_request request;
	struct runtime_result turn;
	struct session session;
	char session_name[256];
	char *task_prompt;
	morph_buf_t prompt;
	int rc;

	if (!ctx || !task || !result)
		return -EINVAL;
	task_prompt = runtime_scheduled_task_prompt(task);
	if (!task_prompt)
		return -EINVAL;
	if (morph_buf_init(&prompt, 1024) != 0) {
		free(task_prompt);
		return -ENOMEM;
	}
	(void)morph_buf_printf(&prompt,
			       "Run this scheduled task now.\n\nTask title: %s\n\n"
			       "Task prompt:\n%s\n\nReturn the result as a concise notification. "
			       "Do not ask follow-up questions.",
			       task->title, task_prompt);
	free(task_prompt);
	snprintf(session_name, sizeof(session_name), "Task #%lld: %.220s", (long long)task->id,
		 task->title);
	rc = runtime_session_create_detached(ctx->runtime, session_name, &session);
	if (rc != 0)
		goto out_cleanup_prompt;
	memset(&request, 0, sizeof(request));
	request.session_id = session.id;
	request.model_input = morph_buf_cstr(&prompt);
	request.stored_user_input = request.model_input;
	request.turn_flags = AGENT_TURN_DEFAULT_FLAGS;
	rc = runtime_execute_turn(ctx->runtime, &request, &turn);
	result->session_id = session.id;
	if (rc == 0)
		result->body = strdup(turn.final_text ? turn.final_text : "");
	else
		result->error = strdup(turn.final_text && turn.final_text[0] ? turn.final_text
									     : morph_strerror(rc));
	if ((rc == 0 && !result->body) || (rc != 0 && !result->error))
		rc = -ENOMEM;
out_cleanup_prompt:
	morph_buf_cleanup(&prompt);
	return rc;
}

static void cli_scheduler_task_notification(const struct notification *notification,
					    void *user_data)
{
	struct cli_application *ctx = user_data;
	if (!notification)
		return;
	if (ctx && cli_event_queue_post_notification(ctx, notification) != 0)
		log_warn(ctx ? ctx->logger : NULL, "failed to queue task notification");
}

void cli_scheduler_process_due(struct cli_application *ctx)
{
	int rc;
	if (!ctx || runtime_task_scheduler_running(ctx->runtime))
		return;
	rc = runtime_tasks_run_due_for_runtime(ctx->runtime, 50, cli_scheduler_run_task, ctx,
					       cli_scheduler_task_notification, ctx);
	if (rc < 0)
		log_warn(ctx ? ctx->logger : NULL, "failed to process due tasks: %s",
			 morph_strerror(rc));
}

int cli_scheduler_start(struct cli_application *ctx)
{
	if (!ctx)
		return -EINVAL;
	return runtime_task_scheduler_start(ctx->runtime, cli_scheduler_run_task, ctx,
					    cli_scheduler_task_notification, ctx, 50);
}
