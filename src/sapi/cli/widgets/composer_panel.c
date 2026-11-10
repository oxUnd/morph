#include "sapi/cli/widgets/composer_panel.h"

#include "sapi/cli/application.h"
#include "sapi/cli/presentation/output.h"
#include "util/array.h"
#include "util/cleanup.h"
#include "util/error.h"
#include "util/utf8.h"
#include <morph/tui/application.h>
#include <morph/tui/controller.h>
#include <morph/tui/draw.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct cli_composer_panel {
	struct cli_application *application;
	struct cli_composer_panel_options options;
	struct cli_composer_panel_ops ops;
	void *data;
	morph_array_t widths;
};

static void cli_composer_panel_frame_cleanup(struct cli_composer_panel_frame *frame)
{
	morph_buf_cleanup(&frame->content);
}

static size_t cli_composer_panel_sgr_length(const char *text)
{
	if (text[0] != '\033' || text[1] != '[')
		return 0;
	size_t i = 2;
	while (i < 96 && text[i] &&
	       ((text[i] >= '0' && text[i] <= '9') || text[i] == ';' || text[i] == ':'))
		i++;
	return text[i] == 'm' ? i + 1 : 0;
}

static int cli_composer_panel_append_line(struct cli_composer_panel_frame *frame, const char *text,
					  int pad)
{
	if (!frame || !text || frame->columns < 0 || frame->rows < 0)
		return -EINVAL;
	if (frame->line_count >= frame->rows)
		return 0;
	size_t length = strlen(text);
	size_t column = 0;
	int rc = 0;
	for (size_t i = 0; rc == 0 && i < length;) {
		size_t sgr = cli_composer_panel_sgr_length(text + i);
		if (sgr) {
			if (frame->color)
				rc = morph_buf_append(&frame->content, text + i, sgr);
			i += sgr;
			continue;
		}
		unsigned codepoint;
		size_t bytes;
		if (!utf8_decode_codepoint(text + i, length - i, &codepoint, &bytes)) {
			frame->content.failed = -EILSEQ;
			return -EILSEQ;
		}
		int width = utf8_codepoint_width(codepoint);
		if (width > 0 && (size_t)width > (size_t)frame->columns - column)
			break;
		rc = morph_buf_append(&frame->content, text + i, bytes);
		if (width > 0)
			column += (size_t)width;
		i += bytes;
	}
	if (rc == 0 && pad && column < (size_t)frame->columns)
		rc = morph_buf_printf(&frame->content, "%*s", frame->columns - (int)column, "");
	if (rc == 0)
		rc = morph_buf_puts(&frame->content, "\033[0m\n");
	if (rc == 0)
		frame->line_count++;
	return rc;
}

int cli_composer_panel_line(struct cli_composer_panel_frame *frame, const char *text,
			    const char *style)
{
	if (!frame || !text)
		return -EINVAL;
	char *safe MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(
		text, strlen(text), UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	morph_buf_t line MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = safe ? morph_buf_init(&line, 128) : -ENOMEM;
	for (size_t i = 0; rc == 0 && style && style[i];) {
		size_t length = cli_composer_panel_sgr_length(style + i);
		if (length && frame->color)
			rc = morph_buf_append(&line, style + i, length);
		i += length ? length : 1;
	}
	if (rc == 0)
		rc = morph_buf_puts(&line, safe);
	if (rc == 0)
		rc = cli_composer_panel_append_line(frame, morph_buf_cstr_borrow(&line), 1);
	if (rc != 0)
		frame->content.failed = rc;
	return rc;
}

int cli_composer_panel_styled_line(struct cli_composer_panel_frame *frame, const char *text)
{
	if (!frame || !text)
		return -EINVAL;
	morph_buf_t safe MORPH_CLEANUP(morph_buf_cleanup) = {0};
	struct utf8_terminal_sanitizer sanitizer;
	utf8_terminal_sanitizer_init(&sanitizer, UTF8_TERMINAL_TEXT_SINGLE_LINE);
	int rc = morph_buf_init(&safe, 128);
	size_t start = 0, length = strlen(text);
	for (size_t i = 0; rc == 0 && i < length;) {
		size_t sgr = cli_composer_panel_sgr_length(text + i);
		if (!sgr) {
			i++;
			continue;
		}
		rc = utf8_terminal_sanitize_feed(&sanitizer, &safe, text + start, i - start, 0);
		if (rc == 0 && frame->color)
			rc = morph_buf_append(&safe, text + i, sgr);
		i += sgr;
		start = i;
	}
	if (rc == 0)
		rc = utf8_terminal_sanitize_feed(&sanitizer, &safe, text + start, length - start,
						 1);
	if (rc == 0)
		rc = cli_composer_panel_append_line(frame, morph_buf_cstr_borrow(&safe), 0);
	if (rc != 0)
		frame->content.failed = rc;
	return rc;
}

static int cli_composer_panel_old_rows(const struct cli_composer_panel *panel, int height)
{
	/* A resize may truncate instead of reflowing. Never infer extra owned rows. */
	size_t rows = panel->widths.nelts;
	if (height > 0 && rows >= (size_t)height)
		rows = (size_t)height - 1;
	return rows <= INT_MAX ? (int)rows : INT_MAX;
}

static int cli_composer_panel_erase(morph_buf_t *output, int rows)
{
	int rc = morph_buf_puts(output, "\033[0m\r");
	if (rows && rc == 0)
		rc = morph_buf_printf(output, "\033[%dA", rows);
	for (int i = 0; rc == 0 && i <= rows; i++) {
		rc = morph_buf_puts(output, "\033[2K");
		if (i < rows && rc == 0)
			rc = morph_buf_puts(output, "\033[B\r");
	}
	return rows && rc == 0 ? morph_buf_printf(output, "\033[%dA\r", rows) : rc;
}

static int cli_composer_panel_draw(void *opaque, const struct morph_tui_screen *screen)
{
	struct cli_composer_panel *panel = opaque;
	struct cli_composer_panel_frame frame MORPH_CLEANUP(cli_composer_panel_frame_cleanup) = {0};
	morph_buf_t output MORPH_CLEANUP(morph_buf_cleanup) = {0};
	morph_array_t widths MORPH_CLEANUP(morph_array_cleanup) = {0};
	int height = screen->area.height;
	int budget = height >= 5 ? height - 4 : 1;
	frame.columns = screen->area.width > 1 ? screen->area.width - 1 : 0;
	if (panel->options.max_columns && frame.columns > panel->options.max_columns)
		frame.columns = panel->options.max_columns;
	frame.rows = budget < panel->options.max_rows ? budget : panel->options.max_rows;
	frame.color = cli_output_color_enabled(panel->application);
	int rc = morph_buf_init(&frame.content, BUFSIZ);
	if (rc == 0)
		rc = morph_buf_init(&output, BUFSIZ);
	size_t width_sample = 0;
	if (rc == 0)
		rc = morph_array_init(&widths, (size_t)frame.rows, sizeof(width_sample));
	if (rc == 0)
		rc = panel->ops.draw(panel->data, &frame);
	if (rc == 0 && frame.content.failed)
		rc = frame.content.failed;
	if (rc == 0)
		rc = morph_buf_puts(&output, "\033[?2026h");
	if (rc == 0)
		rc = cli_composer_panel_erase(&output, cli_composer_panel_old_rows(panel, height));
	const char *line = morph_buf_cstr_borrow(&frame.content);
	for (int i = 0; rc == 0 && i < frame.line_count; i++) {
		size_t length = strcspn(line, "\n");
		morph_buf_t text MORPH_CLEANUP(morph_buf_cleanup) = {0};
		rc = morph_buf_init(&text, length + 1);
		if (rc == 0)
			rc = morph_buf_append(&text, line, length);
		size_t *width = rc == 0 ? morph_array_push_borrow(&widths) : NULL;
		if (rc == 0 && !width)
			rc = -ENOMEM;
		if (rc == 0) {
			*width = utf8_display_width_ansi(morph_buf_cstr_borrow(&text));
			rc = morph_buf_append(&output, line, length);
		}
		if (rc == 0)
			rc = morph_buf_puts(&output, "\033[0m\033[K\r\n");
		line += length + (line[length] == '\n');
	}
	if (rc == 0)
		rc = morph_buf_puts(&output, "\033[?2026l");
	if (rc == 0)
		rc = morph_tui_draw_write(stdout, &output);
	if (rc == 0) {
		morph_array_cleanup(&panel->widths);
		rc = morph_array_take(&panel->widths, &widths);
	}
	return rc;
}

static int cli_composer_panel_event(void *opaque, struct morph_tui_application *application,
				    const struct morph_tui_event *event)
{
	struct cli_composer_panel *panel = opaque;
	int rc = panel->ops.update(panel->data, event);
	if (rc == MORPH_TUI_SCREEN_CLOSE)
		return morph_tui_application_dismiss_controller(application, 0);
	if (rc == MORPH_TUI_SCREEN_REDRAW)
		morph_tui_application_invalidate(application, MORPH_TUI_INVALIDATE_DISPLAY);
	return rc < 0 ? rc : 1;
}

static void cli_composer_panel_disappear(void *opaque, struct morph_tui_application *application)
{
	struct cli_composer_panel *panel = opaque;
	(void)application;
	if (!panel->widths.nelts)
		return;
	int columns, height;
	morph_tui_terminal_dimensions(fileno(stdout), &columns, &height);
	morph_buf_t output MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_buf_init(&output, 128);
	if (rc == 0)
		rc = morph_buf_puts(&output, "\033[?2026h");
	if (rc == 0)
		rc = cli_composer_panel_erase(&output, cli_composer_panel_old_rows(panel, height));
	if (rc == 0)
		rc = morph_buf_puts(&output, "\033[?2026l");
	if (rc == 0)
		(void)morph_tui_draw_write(stdout, &output);
	morph_array_clear(&panel->widths);
}

static void cli_composer_panel_close(void *opaque, int result)
{
	struct cli_composer_panel *panel = opaque;
	cli_application_panel_complete(panel->application, panel->data, panel->ops.complete,
				       result);
	morph_array_cleanup(&panel->widths);
	free(panel);
}

int cli_composer_panel_show(struct cli_application *application,
			    const struct cli_composer_panel_options *options,
			    const struct cli_composer_panel_ops *ops, void *data)
{
	if (!application || !options || options->max_rows < 1 || options->max_columns < 0 ||
	    options->tick_ms < 0 || !ops || !ops->draw || !ops->update || !ops->complete)
		return -EINVAL;
	struct cli_composer_panel *panel = calloc(1, sizeof(*panel));
	if (!panel)
		return -ENOMEM;
	panel->application = application;
	panel->options = *options;
	panel->ops = *ops;
	panel->data = data;
	static const struct morph_tui_controller_ops cli_composer_panel_controller_ops = {
		.draw = cli_composer_panel_draw,
		.event = cli_composer_panel_event,
		.disappear = cli_composer_panel_disappear,
		.close = cli_composer_panel_close,
	};
	struct morph_tui_controller controller = {
		.ops = &cli_composer_panel_controller_ops,
		.data = panel,
		.input_mode = MORPH_TUI_INPUT_KEYS,
		.terminal_flags = MORPH_TUI_TERMINAL_SESSION_PASTE,
		.tick_ms = options->tick_ms,
	};
	int rc = morph_tui_application_present_controller_take(
		cli_application_tui_borrow(application), &controller);
	if (rc != 0)
		free(panel);
	return rc;
}
