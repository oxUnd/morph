#include "sapi/cli/presentation/jobs.h"
#include <nb/err.h>
#include "sapi/cli/application.h"
#include "sapi/cli/models/jobs.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/presentation/feed.h"
#include "sapi/cli/presentation/feed_block.h"
#include "sapi/cli/presentation/list.h"
#include "sapi/cli/presentation/notice.h"
#include "sapi/cli/presentation/work_status.h"
#include "cJSON.h"
#include <nb/cleanup.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <nb/utf8.h>
#include <nothing/tui/terminal/text.h>

static int cli_jobs_ensure(struct cli_application *ctx)
{
	if (ctx->jobs)
		return 0;

	return cli_jobs_model_create(&ctx->jobs);
}

int cli_jobs_event(struct cli_application *ctx, const struct morph_event *event)
{
	int rc;

	if (!ctx || !event)
		return NB_ERR_INVAL;

	if (event->name && (!strcmp(event->name, "background.wait.begin") ||
			    !strcmp(event->name, "background.wait.end"))) {
		cli_jobs_model_note_wait(ctx->jobs, cli_jobs_job_string(event->data, "session_id"),
					 !strcmp(event->name, "background.wait.begin"));
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
				nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
				const char *status = cli_jobs_job_string(event->data, "status");
				int timeout = strstr(status, "timeout") || strstr(status, "timed_out");
				const cJSON *exit_code =
					cJSON_GetObjectItemCaseSensitive(event->data, "exit_code");

				(void)nb_buf_printf(&title, "%s · %s", safe,
						    timeout ? "TIMEOUT" : status);
				if (cJSON_IsNumber(exit_code))
					(void)nb_buf_printf(&title, " %d", exit_code->valueint);

				(void)nb_buf_printf(&title, " · /jobs %.8s",
						    cli_jobs_job_short_id(event->data));
				(void)cli_feed_notice_write(
					ctx,
					cli_jobs_job_cancelled(event->data) ? CLI_NOTICE_CANCELLED
					: cli_jobs_job_failed(event->data)  ? CLI_NOTICE_FAILED
									    : CLI_NOTICE_DONE,
					nb_buf_cstr(&title), NULL, NULL);
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

static int cli_jobs_job_row(nb_buf *out, const char *prefix, const char *text, const char *meta,
			    int columns, const char *style, const char *prefix_style)
{
	char compact[BUFSIZ], clipped[BUFSIZ];
	int padding;
	int budget;

	if (cli_list_compact_text(compact, sizeof(compact), text) == SIZE_MAX)
		return NB_ERR_NOMEM;

	budget = columns - 1 - (int)nb_utf8_width_cstr(prefix) - (int)nb_utf8_width_cstr(meta) -
		 (meta[0] ? 2 : 0);
	if (budget < 1)
		budget = 1;

	nt_terminal_copy_ellipsized_display_width(clipped, sizeof(clipped), compact,
						     (size_t)budget, 0);
	if (out->len)
		(void)nb_buf_append_char(out, '\n');

	padding = budget - (int)nb_utf8_width_cstr(clipped);
	return (nb_buf_printf(out, "%s%s%s%s%s%*s%s%s", prefix_style ? prefix_style : style, prefix,
			      prefix_style ? ANSI_RESET : "", prefix_style ? style : "", clipped,
			      meta[0] ? padding + 2 : 0, "", meta, style[0] ? ANSI_RESET : "")
			? 0
			: NB_ERR_NOMEM);
}

int cli_jobs_activity_render(const struct cli_jobs *jobs, nb_buf *summary, nb_buf *details,
	int columns, int styled)
{
	int count = cli_jobs_model_count(jobs);
	if (!count)
		return 0;

	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!nb_buf_printf(&text, "%d background job%s running · /jobs", count,
		count == 1 ? "" : "s"))
		return nb_buf_status(&text);

	int rc = cli_work_status_detail(summary, nb_buf_cstr(&text), columns, styled);
	const struct cli_jobs_job *waiting = cli_jobs_model_waiting(jobs);
	if (!rc && waiting)
		rc = cli_work_status_detail(details, cli_jobs_job_string(waiting->data, "command"),
			columns, styled);

	return rc;
}

int cli_jobs_render(const struct cli_jobs *jobs, nb_buf *out, int columns, int styled,
		    int64_t now)
{
	int rc = nb_buf_status(out);
	if (rc)
		return rc;

	int total = cli_jobs_model_count(jobs);
	if (!total || columns < 20)
		return 0;

	const struct cli_jobs_job *waiting = cli_jobs_model_waiting(jobs);
	nb_buf summary NB_CLEANUP(nb_buf_cleanup) = {0};
	if (waiting) {
		int64_t seconds = cli_jobs_model_wait_elapsed(jobs, now) / 1000;
		(void)nb_buf_append_cstr(&summary, columns < 60 ? "Waiting for job · "
								: "Waiting for background job (");
		if (seconds >= 60)
			(void)nb_buf_printf(&summary, "%lldm ", (long long)(seconds / 60));

		(void)nb_buf_printf(&summary, "%llds", (long long)(seconds % 60));
		(void)nb_buf_append_cstr(&summary, columns < 60	  ? " · Esc"
						   : columns < 90 ? " · Esc)"
								  : " · Esc to interrupt wait)");
		if (columns >= 100)
			(void)nb_buf_printf(&summary, " · %d running", total);
	} else {
		(void)nb_buf_printf(&summary, "%d background job%s%s", total, total == 1 ? "" : "s",
				    columns < 60 ? "" : " running");
	}

	(void)nb_buf_append_cstr(&summary, columns < 100 ? " · /jobs" : " · /jobs to view");
	rc = nb_buf_status(&summary);
	if (!rc)
		rc = cli_jobs_job_row(out, CLI_FEED_RUNNING " ", nb_buf_cstr(&summary), "", columns,
				      styled ? ANSI_CYAN : "",
				      styled ? cli_style_running_color(now) : NULL);

	if (!rc && waiting)
		rc = cli_jobs_job_row(out, CLI_ACTIVITY_CHILD_PREFIX,
				      cli_jobs_job_string(waiting->data, "command"), "", columns,
				      styled ? ANSI_DIM : "",
				      styled ? CLI_ACTIVITY_CONNECTOR_COLOR : NULL);

	return rc ? rc : waiting ? 2 : 1;
}
