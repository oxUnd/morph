#include "sapi/cli/widgets/jobs.h"
#include "sapi/cli/models/jobs.h"

#include "sapi/cli/presentation/style.h"
#include "sapi/cli/widgets/list.h"
#include "util/cleanup.h"
#include "util/utf8.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

static int cli_jobs_job_row(morph_buf_t *out, const char *prefix, const char *text,
			    const char *meta, int columns, const char *style,
			    const char *prefix_style)
{
	char compact[BUFSIZ], clipped[BUFSIZ];
	int padding;
	int budget;

	cli_list_compact_text(compact, sizeof(compact), text);
	budget = columns - 1 - (int)utf8_display_width(prefix) -
		 (int)utf8_display_width(meta) - (meta[0] ? 2 : 0);
	if (budget < 1)
		budget = 1;
	utf8_copy_ellipsized_display_width(clipped, sizeof(clipped), compact, (size_t)budget, 0);
	if (out->len)
		(void)morph_buf_putc(out, '\n');
	padding = budget - (int)utf8_display_width(clipped);
	return morph_buf_printf(out, "%s%s%s%s%s%*s%s%s",
				prefix_style ? prefix_style : style, prefix,
				prefix_style ? ANSI_RESET : "", prefix_style ? style : "", clipped,
				meta[0] ? padding + 2 : 0, "", meta, style[0] ? ANSI_RESET : "");
}

int cli_jobs_render(const struct cli_jobs *jobs, morph_buf_t *out, int columns, int styled)
{
	const struct cli_jobs_job *waiting = NULL;
	const struct cli_jobs_job *job;
	int total = 0;
	morph_buf_t summary MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int64_t elapsed;
	int64_t seconds;

	if (!jobs || !out || columns < 20)
		return 0;
	morph_array_foreach (job, &jobs->entries, struct cli_jobs_job) {
		if (cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
			    job->data, "owner_session_id")) != (double)jobs->session_id ||
		    !cli_jobs_job_running(job->data))
			continue;
		total++;
		if (job->waiting && !waiting)
			waiting = job;
	}
	if (!total)
		return 0;
	if (morph_buf_init(&summary, 128))
		return 0;
	if (waiting) {
		elapsed = (int64_t)cJSON_GetNumberValue(
			cJSON_GetObjectItemCaseSensitive(waiting->data, "duration_ms"));
		seconds = (elapsed + cli_jobs_now() - waiting->updated) / 1000;
		(void)morph_buf_puts(&summary, columns < 60 ? "Waiting for job · "
							   : "Waiting for background job (");
		if (seconds >= 60)
			(void)morph_buf_printf(&summary, "%lldm ", (long long)(seconds / 60));
		(void)morph_buf_printf(&summary, "%llds", (long long)(seconds % 60));
		(void)morph_buf_puts(&summary, columns < 60 ? " · Esc" :
			columns < 90 ? " · Esc)" : " · Esc to interrupt wait)");
		if (columns >= 100)
			(void)morph_buf_printf(&summary, " · %d running", total);
	} else {
		(void)morph_buf_printf(&summary, "%d background job%s%s", total,
				       total == 1 ? "" : "s", columns < 60 ? "" : " running");
	}
	(void)morph_buf_puts(&summary, columns < 100 ? " · /jobs" : " · /jobs to view");
	(void)cli_jobs_job_row(out, CLI_FEED_RUNNING " ", morph_buf_cstr_borrow(&summary), "",
			       columns, styled ? ANSI_CYAN : "",
			       styled ? cli_style_running_color(cli_jobs_now()) : NULL);
	if (!waiting)
		return 1;
	(void)cli_jobs_job_row(out, "  └ ", cli_jobs_job_string(waiting->data, "command"), "",
			       columns, styled ? ANSI_DIM : "", NULL);
	return 2;
}
