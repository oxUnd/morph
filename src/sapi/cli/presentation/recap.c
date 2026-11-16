#include "sapi/cli/presentation/recap.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/services/recap.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/feed_block.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cli_recap_append_paragraph(nb_buf *out, const char *text, int columns,
				      int styled, int heading)
{
	char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		text, strlen(text), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;
	const char *label = heading ? "  ↳ Recap: " : "           ";
	size_t indent = nb_utf8_width_cstr(label);
	size_t width = (size_t)columns;
	const char *line = safe;
	int first = 1;
	if (width <= indent + 1) {
		if (heading) {
			const char *end = morph_terminal_advance_display_width_borrow(label, width);
			(void)nb_buf_printf(out, "%s%.*s%s\n", styled ? CLI_RECAP_COLOR : "",
				(int)(end - label), label, styled ? ANSI_RESET : "");
		}
		indent = 0;
		first = 0;
	}
	while (*line) {
		const char *end = morph_terminal_advance_display_width_borrow(line, width - indent);
		int clipped = end == line;
		if (clipped)
			end = nb_utf8_advance(line, 1);
		if (styled)
			(void)nb_buf_append_cstr(out, CLI_RECAP_COLOR);
		if (first && heading && styled)
			(void)nb_buf_append_cstr(out,
				"  ↳ " ANSI_BOLD "Recap:" ANSI_RESET CLI_RECAP_COLOR " ");
		else if (first)
			(void)nb_buf_append_cstr(out, label);
		else
			(void)nb_buf_printf(out, "%*s", (int)indent, "");
		if (clipped)
			(void)nb_buf_append_cstr(out, "…");
		else
			(void)nb_buf_append(out, line, (size_t)(end - line));
		(void)nb_buf_append_cstr(out, styled ? ANSI_RESET "\n" : "\n");
		if (!nb_buf_ok(out))
			return nb_buf_status(out);
		first = 0;
		line = end;
		while (*line == ' ')
			line++;
	}
	return nb_buf_status(out);
}

int cli_recap_append(nb_buf *out, const char *summary, const char *next, int columns, int styled)
{
	if (!out || !summary)
		return NB_ERR_INVAL;
	if (columns <= (int)CLI_CONTENT_RIGHT_PADDING)
		return 0;
	int width = columns - (int)CLI_CONTENT_RIGHT_PADDING;
	int rc = cli_recap_append_paragraph(out, summary, width, styled, 1);
	if (rc || !next || !*next)
		return rc;
	nb_buf action NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!nb_buf_printf(&action, "Next: %s", next))
		return nb_buf_status(&action);
	return cli_recap_append_paragraph(out, nb_buf_cstr(&action), width, styled, 0);
}

/* Wrap sanitized recap text under a fixed label, one line at a time. */
static void cli_recap_print_text(struct cli_application *ctx, const char *label,
				 const char *text)
{
	char *safe = morph_terminal_sanitize_dup(text, strlen(text),
						 MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
	const char *line = safe;
	size_t columns = (size_t)cli_feed_columns(ctx);
	size_t indent = nb_utf8_width_cstr(label);
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
		const char *end = morph_terminal_advance_display_width_borrow(line, width);

		if (end == line)
			end = nb_utf8_advance(line, 1);
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
		struct cli_feed_block block = {
			.kind = CLI_FEED_BLOCK_RECAP,
			.margin = {.top = 1, .bottom = 1},
			.content.recap = {.summary = summary, .next = next},
		};
		(void)cli_feed_block_write(ctx, &block);
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
	if (code == NB_ERR_BUSY)
		cli_output_printf(ctx, CLI_FEED_FAILED_STYLE CLI_FEED_FAILED
				  " Error  " ANSI_RESET
				  "Wait for the current task to finish before running "
				  "/recap.\n");
	else if (code == NB_ERR_STATE)
		cli_output_printf(ctx, CLI_FEED_FAILED_STYLE CLI_FEED_FAILED
				  " Error  " ANSI_RESET
				  "A conversation recap is already being generated.\n");
	else if (code == NB_ERR_NO_DATA)
		cli_output_printf(ctx, CLI_FEED_FAILED_STYLE CLI_FEED_FAILED
				  " Error  " ANSI_RESET
				  "There is no conversation history to recap.\n");
	else
		cli_output_printf(ctx,
				  CLI_FEED_FAILED_STYLE CLI_FEED_FAILED
				  " Error  " ANSI_RESET "Could not generate a recap: %s\n",
				  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, code));
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
