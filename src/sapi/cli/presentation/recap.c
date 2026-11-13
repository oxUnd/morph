#include "sapi/cli/presentation/recap.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/services/recap.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/feed_block.h"
#include "util/buf.h"
#include "util/cleanup.h"
#include "util/error.h"
#include "util/utf8.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Wrap sanitized recap text under a fixed label, one line at a time. */
static void cli_recap_print_text(struct cli_application *ctx, const char *label,
				 const char *text)
{
	char *safe = utf8_terminal_sanitize_dup(text, strlen(text),
						UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	const char *line = safe;
	size_t columns = (size_t)cli_feed_columns(ctx);
	size_t indent = utf8_display_width(label);
	int first = 1;

	if (!safe)
		return;
	if (columns <= indent + 3) {
		cli_output_printf(ctx, "%s\n", label);
		indent = 0;
		first = 0;
	}
	while (*line) {
		size_t width = columns > indent + 1 ? columns - indent - 1 : 2;
		const char *end = utf8_advance_display_width_borrow(line, width);

		if (end == line)
			end = utf8_advance_codepoints_borrow(line, 1);
		cli_output_printf(ctx, "%s%.*s%.*s\n", first ? label : "",
				  first ? 0 : (int)indent,
				  "                                  ",
				  (int)(end - line), line);
		first = 0;
		line = end;
		while (*line == ' ')
			line++;
	}
	free(safe);
}

static void cli_recap_render_show(struct cli_application *ctx, const char *summary,
				  const char *next)
{
	if (!ctx || !summary)
		return;
	cli_feed_history_begin(ctx);
	if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE) {
		morph_buf_t footer MORPH_CLEANUP(morph_buf_cleanup) = {0};

		if (next && *next)
			(void)morph_buf_printf(&footer, "Next: %s", next);
		(void)cli_feed_notice_write(ctx, CLI_NOTICE_SYSTEM, "Recap:", summary,
					    morph_buf_cstr_borrow(&footer));
	} else {
		if (cli_output_color_enabled(ctx))
			cli_output_printf(ctx, ANSI_DIM "\033[3m");
		cli_recap_print_text(ctx, "  ↳ Recap: ", summary);
		if (next && next[0])
			cli_recap_print_text(ctx, "    Next: ", next);
		if (cli_output_color_enabled(ctx))
			cli_output_printf(ctx, ANSI_RESET);
		cli_output_printf(ctx, "\n");
	}
	fflush(stdout);
	cli_feed_history_end(ctx);
}

static void cli_recap_render_failed(struct cli_application *ctx, int code)
{
	if (!ctx)
		return;
	if (code == -EBUSY)
		cli_output_printf(ctx, CLI_FEED_FAILED_STYLE CLI_FEED_FAILED
				  " Error  " ANSI_RESET
				  "Wait for the current task to finish before running "
				  "/recap.\n");
	else if (code == -EALREADY)
		cli_output_printf(ctx, CLI_FEED_FAILED_STYLE CLI_FEED_FAILED
				  " Error  " ANSI_RESET
				  "A conversation recap is already being generated.\n");
	else if (code == -ENODATA)
		cli_output_printf(ctx, CLI_FEED_FAILED_STYLE CLI_FEED_FAILED
				  " Error  " ANSI_RESET
				  "There is no conversation history to recap.\n");
	else
		cli_output_printf(ctx, CLI_FEED_FAILED_STYLE CLI_FEED_FAILED
				  " Error  " ANSI_RESET
				  "Could not generate a recap: %s\n",
				  morph_strerror(code));
}

static const struct cli_recap_renderer cli_recap_presentation_renderer = {
	.show = cli_recap_render_show,
	.failed = cli_recap_render_failed,
};

void cli_presentation_recap_attach(struct cli_application *ctx)
{
	if (!ctx)
		return;
	cli_recap_set_renderer(ctx, &cli_recap_presentation_renderer);
}
