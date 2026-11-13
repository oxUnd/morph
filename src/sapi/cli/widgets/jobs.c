#include "sapi/cli/widgets/jobs.h"
#include "sapi/cli/models/jobs.h"

#include "sapi/cli/presentation/style.h"
#include "sapi/cli/widgets/list.h"
#include "nb/cleanup.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

static int cli_jobs_job_row(nb_buf *out, const char *prefix, const char *text, const char *meta,
			    int columns, const char *style, const char *prefix_style)
{
	char compact[BUFSIZ], clipped[BUFSIZ];
	int padding;
	int budget;

	cli_list_compact_text(compact, sizeof(compact), text);
	budget = columns - 1 - (int)nb_utf8_width_cstr(prefix) - (int)nb_utf8_width_cstr(meta) -
		 (meta[0] ? 2 : 0);
	if (budget < 1)
		budget = 1;
	morph_terminal_copy_ellipsized_display_width(clipped, sizeof(clipped), compact,
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

int cli_jobs_render(const struct cli_jobs *jobs, nb_buf *out, int columns, int styled)
{
	const struct cli_jobs_job *waiting = NULL;
	const struct cli_jobs_job *job;
	int total = 0;
	nb_buf summary NB_CLEANUP(nb_buf_cleanup) = {0};
	int64_t elapsed;
	int64_t seconds;

	if (!jobs || !out || columns < 20)
		return 0;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&jobs->entries) &&
			     (job = (struct cli_jobs_job *)nb_vec_at(&jobs->entries, _i_));
	     _i_++) {
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
	if ((nb_buf_init(&summary), nb_buf_reserve(&summary, 128) ? 0 : NB_ERR_NOMEM))
		return 0;
	if (waiting) {
		elapsed = (int64_t)cJSON_GetNumberValue(
			cJSON_GetObjectItemCaseSensitive(waiting->data, "duration_ms"));
		seconds = (elapsed + cli_jobs_now() - waiting->updated) / 1000;
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
	(void)cli_jobs_job_row(out, CLI_FEED_RUNNING " ", nb_buf_cstr(&summary), "", columns,
			       styled ? ANSI_CYAN : "",
			       styled ? cli_style_running_color(cli_jobs_now()) : NULL);
	if (!waiting)
		return 1;
	(void)cli_jobs_job_row(out, "  └ ", cli_jobs_job_string(waiting->data, "command"), "",
			       columns, styled ? ANSI_DIM : "", NULL);
	return 2;
}
