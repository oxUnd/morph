#ifndef MORPH_CLI_SCHEDULER_H
#define MORPH_CLI_SCHEDULER_H

#include "db/scheduled_task.h"

struct cli_context;

void cli_process_due_tasks(struct cli_context *ctx);
int cli_scheduled_task_runner(const struct scheduled_task *task,
			      struct scheduled_task_action_result *result, void *user_data);
int cli_scheduler_start(struct cli_context *ctx);
int cli_sync_start(struct cli_context *ctx);

#endif
