#ifndef MORPH_CLI_SERVICES_SCHEDULER_H
#define MORPH_CLI_SERVICES_SCHEDULER_H

#include "db/scheduled_task.h"

struct cli_application;

void cli_scheduler_process_due(struct cli_application *ctx);
int cli_scheduler_run_task(const struct scheduled_task *task,
			   struct scheduled_task_action_result *result, void *user_data);
int cli_scheduler_start(struct cli_application *ctx);

#endif
