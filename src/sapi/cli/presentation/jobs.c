#include "sapi/cli/presentation/jobs.h"

#include "sapi/cli/application.h"
#include "sapi/cli/models/jobs.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/feed_block.h"
#include "sapi/cli/widgets/list.h"
#include "sapi/cli/widgets/notice.h"
#include "cJSON.h"
#include "util/cleanup.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int cli_jobs_ensure(struct cli_application *ctx)
{
	if (ctx->jobs)
		return 0;
	return cli_jobs_model_create(&ctx->jobs);
}

int cli_jobs_event(struct cli_application *ctx, const struct morph_event *event)
{
	struct cli_jobs_job *job;
	int rc;

	if (!ctx || !event)
		return -EINVAL;
	if (event->name && (!strcmp(event->name, "background.wait.begin") ||
			    !strcmp(event->name, "background.wait.end"))) {
		if (ctx->jobs) {
			morph_array_foreach (job, &ctx->jobs->entries, struct cli_jobs_job) {
				if (!strcmp(cli_jobs_job_string(job->data, "session_id"),
					    cli_jobs_job_string(event->data, "session_id"))) {
					if (!strcmp(event->name, "background.wait.begin"))
						job->waiting++;
					else if (job->waiting > 0)
						job->waiting--;
				}
			}
		}
		cli_feed_queue_changed(ctx);
		return 1;
	}
	if (!event->name ||
	    (strcmp(event->name, "process.started") && strcmp(event->name, "process.completed")))
		return 0;
	rc = cli_jobs_ensure(ctx);
	if (rc)
		return rc;
	rc = cli_jobs_model_remember(ctx->jobs, event->data);
	if (rc)
		return rc;
	if (!strcmp(event->name, "process.completed")) {
		if (cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
			    event->data, "owner_session_id")) == (double)ctx->jobs->session_id) {
			const char *description = cli_jobs_job_string(event->data, "description");
			char safe[BUFSIZ];
			(void)cli_list_compact_text(
				safe, sizeof(safe),
				description[0] ? description
					       : cli_jobs_job_string(event->data, "command"));
			cli_feed_history_begin(ctx);
			if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE) {
				morph_buf_t title MORPH_CLEANUP(morph_buf_cleanup) = {0};
				const char *status = cli_jobs_job_string(event->data, "status");
				int timeout = strstr(status, "timeout") || strstr(status, "timed_out");
				const cJSON *exit_code =
					cJSON_GetObjectItemCaseSensitive(event->data, "exit_code");

				(void)morph_buf_printf(&title, "%s · %s", safe,
						       timeout ? "TIMEOUT" : status);
				if (cJSON_IsNumber(exit_code))
					(void)morph_buf_printf(&title, " %d", exit_code->valueint);
				(void)morph_buf_printf(&title, " · /jobs %.8s",
						       cli_jobs_job_short_id(event->data));
				(void)cli_feed_notice_write(
					ctx, cli_jobs_job_cancelled(event->data) ? CLI_NOTICE_CANCELLED
					     : cli_jobs_job_failed(event->data) ? CLI_NOTICE_FAILED
									       : CLI_NOTICE_DONE,
					morph_buf_cstr_borrow(&title), NULL, NULL);
			} else {
				const cJSON *exit_code =
					cJSON_GetObjectItemCaseSensitive(event->data, "exit_code");

				cli_output_printf(ctx, "%s%s" ANSI_RESET " %s · %s",
				       cli_jobs_job_cancelled(event->data) ? ANSI_DIM
				       : cli_jobs_job_failed(event->data) ? CLI_FEED_FAILED_STYLE
									 : ANSI_GREEN,
				       cli_jobs_job_cancelled(event->data) ? CLI_FEED_CANCELLED
				       : cli_jobs_job_failed(event->data) ? CLI_FEED_FAILED
									 : CLI_FEED_DONE,
				       safe, cli_jobs_job_string(event->data, "status"));
				if (cJSON_IsNumber(exit_code))
					cli_output_printf(ctx, " %d", exit_code->valueint);
				cli_output_printf(ctx, " · /jobs %.8s\n", cli_jobs_job_short_id(event->data));
			}
			cli_feed_history_end(ctx);
		}
	}
	cli_feed_queue_changed(ctx);
	return 1;
}
