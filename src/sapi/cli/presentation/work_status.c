#include "sapi/cli/presentation/work_status.h"
#include "nb/err.h"
#include "sapi/cli/presentation/style.h"
#include "nb/cleanup.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int cli_work_status_label(nb_buf *out, int styled, int64_t elapsed, int64_t now)
{
	const char label[] = "Working";
	if (!styled)
		return (nb_buf_append_cstr(out, CLI_FEED_RUNNING " Working") ? 0 : NB_ERR_NOMEM);
	int rc = (nb_buf_printf(out, ANSI_RESET "%s" CLI_FEED_RUNNING ANSI_RESET " ",
				cli_style_running_color(now))
			  ? 0
			  : NB_ERR_NOMEM);
	int64_t phase = elapsed > 600 ? (elapsed - 600) % 4000 : 0;
	double position = (double)(phase < 1000 ? phase : 1000) / 1000.0 * 13.0 - 3.0;
	for (size_t i = 0; !rc && i < sizeof(label) - 1; i++) {
		double distance = ((double)i + 0.5 - position) / 3.0;
		if (distance < 0.0)
			distance = -distance;
		double intensity = distance < 1.0 ? 1.0 - distance : 0.0;
		int shade = 138 + (int)(117.0 * intensity * intensity * (3.0 - 2.0 * intensity));
		rc = (nb_buf_printf(out, "\033[38;2;%d;%d;%dm%c", shade, shade, shade, label[i])
			      ? 0
			      : NB_ERR_NOMEM);
	}
	return rc ? rc : (nb_buf_append_cstr(out, ANSI_RESET) ? 0 : NB_ERR_NOMEM);
}

static int cli_work_status_duration(nb_buf *out, int64_t elapsed)
{
	int64_t seconds = elapsed > 0 ? elapsed / 1000 : 0;
	int rc = 0;
	if (seconds >= 60)
		rc = (nb_buf_printf(out, "%lldm ", (long long)(seconds / 60)) ? 0 : NB_ERR_NOMEM);
	return rc ? rc
		  : (nb_buf_printf(out, "%llds", (long long)(seconds % 60)) ? 0 : NB_ERR_NOMEM);
}

static int cli_work_status_text(nb_buf *out, const char *text, int columns, int styled)
{
	char clipped[BUFSIZ];
	(void)morph_terminal_copy_ellipsized_display_width(clipped, sizeof(clipped), text,
							   (size_t)(columns > 0 ? columns : 1), 0);
	return (nb_buf_printf(out, "%s%s%s", styled ? ANSI_DIM : "", clipped,
			      styled ? ANSI_RESET : "")
			? 0
			: NB_ERR_NOMEM);
}

int cli_work_status_running(nb_buf *out, int columns, int styled, int64_t elapsed_ms,
			    int64_t now_ms, const char *agent_summary)
{
	if (!out || columns < 1)
		return NB_ERR_INVAL;
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = cli_work_status_duration(&text, elapsed_ms);
	int summary = agent_summary && *agent_summary;
	if (!rc)
		rc = (nb_buf_append_cstr(&text,
					 summary && columns < 76 ? " · esc" : " · esc to interrupt")
			      ? 0
			      : NB_ERR_NOMEM);
	if (!rc && summary)
		rc = (nb_buf_printf(&text, " · %s", agent_summary) ? 0 : NB_ERR_NOMEM);
	if (rc)
		return rc;
	const char *hint = columns < 76 ? " · Ctrl+T" : " · Ctrl+T transcript";
	int hint_width = columns >= 45 ? (int)nb_utf8_width_cstr(hint) : 0;
	rc = cli_work_status_label(out, styled, elapsed_ms, now_ms);
	if (!rc)
		rc = (nb_buf_append_cstr(out, styled ? ANSI_DIM " · " : " · ") ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = cli_work_status_text(out, nb_buf_cstr(&text), columns - 12 - hint_width,
					  styled);
	if (!rc && hint_width)
		rc = (nb_buf_printf(out, "%s%s%s", styled ? CLI_COMMAND_COLOR : "", hint,
				    styled ? ANSI_RESET : "")
			      ? 0
			      : NB_ERR_NOMEM);
	return rc;
}

int cli_work_status_completed(nb_buf *out, int columns, int styled, int result, int64_t elapsed_ms,
			      time_t finished_at)
{
	if (!out || columns < 1)
		return NB_ERR_INVAL;
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_append_cstr(&text, result >= 0			 ? "Worked for "
					    : result == NB_ERR_CANCELLED ? "Stopped after "
									 : "Failed after ")
			  ? 0
			  : NB_ERR_NOMEM);
	if (!rc)
		rc = cli_work_status_duration(&text, elapsed_ms);
	struct tm finished;
	if (!rc && localtime_r(&finished_at, &finished)) {
		char clock[32] = "";
		(void)strftime(clock, sizeof(clock), "%H:%M", &finished);
		rc = (nb_buf_printf(&text, " · %s", clock) ? 0 : NB_ERR_NOMEM);
	}
	return rc ? rc : cli_work_status_text(out, nb_buf_cstr(&text), columns - 2, styled);
}

int cli_work_status_interval_ms(int64_t elapsed_ms, int64_t now_ms)
{
	int64_t elapsed = elapsed_ms > 0 ? elapsed_ms : 0;
	int64_t phase = elapsed > 600 ? (elapsed - 600) % 4000 : 0;
	int64_t delay = elapsed < 600 ? 600 - elapsed
			: phase < 1000 ? 32
				       : 4000 - phase;
	int64_t second = 1000 - elapsed % 1000;
	int64_t pulse = cli_style_running_interval_ms(now_ms);
	int64_t interval = delay < second ? delay : second;
	return (int)(interval < pulse ? interval : pulse);
}
