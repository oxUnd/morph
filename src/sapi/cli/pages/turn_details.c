#include "sapi/cli/pages/turn_details.h"
#include "sapi/cli/application.h"
#include "sapi/cli/widgets/transcript.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/services/recap.h"
#include "util/error.h"
#include <morph/tui/application.h>
#include <morph/tui/frame.h>
#include <morph/tui/input.h>
#include <morph/tui/terminal.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct cli_turn_details {
	struct cli_application *ctx;
	struct morph_tui_frame frame;
	size_t offset;
	int follow;
	int open;
	int visible;
	int columns;
	int rows;
};

static int cli_turn_details_prepare(struct cli_application *ctx)
{
	if (!ctx->details) {
		ctx->details = calloc(1, sizeof(*ctx->details));
		if (!ctx->details)
			return -ENOMEM;
		ctx->details->ctx = ctx;
		ctx->details->follow = 1;
	}
	return 0;
}

int cli_turn_details_is_open(const struct cli_application *ctx)
{
	return ctx && ctx->details && ctx->details->open;
}

int cli_turn_details_is_visible(const struct cli_application *ctx)
{
	return ctx && ctx->details && ctx->details->visible;
}

static int cli_turn_details_compose(struct cli_turn_details *page, int scroll, FILE *output)
{
	struct cli_application *ctx = page->ctx;
	morph_buf_t body = {0};
	morph_buf_t frame = {0};
	size_t lines = 0;
	int height = page->rows > 5 ? page->rows - 4 : 1;
	int visible = page->visible;
	int rc = morph_buf_init(&body, BUFSIZ);

	page->visible = 1;
	if (!rc)
		rc = cli_transcript_render_details(ctx, ctx->transcript, &body);
	page->visible = visible;
	if (!rc)
		rc = morph_buf_init(&frame, BUFSIZ);
	if (rc) {
		morph_buf_cleanup(&body);
		morph_buf_cleanup(&frame);
		return rc;
	}
	const char *line = morph_buf_cstr_borrow(&body);

	for (const char *p = line; *p; p++)
		lines += *p == '\n';
	if (scroll == INT_MAX)
		page->follow = 1;
	else if (scroll == INT_MIN) {
		page->follow = 0;
		page->offset = 0;
	} else if (scroll) {
		size_t step = (size_t)(scroll < 0 ? -scroll : scroll);

		page->follow = 0;
		if (scroll < 0)
			page->offset = step > page->offset ? 0 : page->offset - step;
		else
			page->offset =
				step > SIZE_MAX - page->offset ? SIZE_MAX : page->offset + step;
	}
	size_t maximum = lines > (size_t)height ? lines - (size_t)height : 0;

	if (page->follow || page->offset > maximum)
		page->offset = maximum;
	page->follow = page->offset == maximum;
	morph_buf_printf(&frame, "%s  Turn details%s  · %zu calls\n\n%s",
			 cli_output_color_enabled(ctx) ? ANSI_BOLD : "",
			 cli_output_color_enabled(ctx) ? ANSI_RESET ANSI_DIM : "",
			 ctx->transcript ? ctx->transcript->tools.nelts : 0,
			 cli_output_color_enabled(ctx) ? ANSI_RESET : "");
	for (size_t i = 0; i < page->offset && *line; i++) {
		const char *next = strchr(line, '\n');

		line = next ? next + 1 : line + strlen(line);
	}
	for (int i = 0; i < height; i++) {
		size_t length = strcspn(line, "\n");

		morph_buf_append(&frame, line, length);
		morph_buf_puts(&frame, ANSI_RESET "\n");
		line += length + (line[length] == '\n');
	}
	morph_buf_putc(&frame, '\n');
	const char *hint = "  Ctrl+T / esc back · PgUp/PgDn scroll · End follow";
	const char *end = utf8_advance_display_width_borrow(
		hint, page->columns > 1 ? (size_t)page->columns - 1 : 0);

	morph_buf_append(&frame, hint, (size_t)(end - hint));
	rc = frame.failed ? frame.failed
			  : morph_tui_frame_present(&page->frame, output, page->columns, page->rows,
						    morph_buf_cstr_borrow(&frame));
	morph_buf_cleanup(&frame);
	morph_buf_cleanup(&body);
	return rc;
}

void cli_turn_details_render(struct cli_application *ctx, int scroll)
{
	if (!ctx || cli_turn_details_prepare(ctx))
		return;
	morph_tui_terminal_dimensions(STDOUT_FILENO, &ctx->details->columns, &ctx->details->rows);
	(void)cli_turn_details_compose(ctx->details, scroll, stdout);
}

static int cli_turn_details_draw(void *data, const struct morph_tui_screen *screen)
{
	struct cli_turn_details *page = data;

	page->columns = screen->area.width;
	page->rows = screen->area.height;
	return cli_turn_details_compose(page, 0, screen->session.output);
}

static int cli_turn_details_event(void *data, struct morph_tui_application *app,
				  const struct morph_tui_event *event)
{
	struct cli_turn_details *page = data;
	int scroll = 0;

	if (event->type == MORPH_TUI_EVENT_KEY && !event->pasting) {
		switch (event->key) {
		case 20:
		case 27:
		case 3:
			return morph_tui_application_dismiss_controller(app, 0);
		case MORPH_TUI_KEY_UP:
			scroll = -1;
			break;
		case MORPH_TUI_KEY_DOWN:
			scroll = 1;
			break;
		case MORPH_TUI_KEY_PAGE_UP:
			scroll = -(page->rows > 4 ? page->rows - 4 : 1);
			break;
		case MORPH_TUI_KEY_PAGE_DOWN:
			scroll = page->rows > 4 ? page->rows - 4 : 1;
			break;
		case MORPH_TUI_KEY_HOME:
			scroll = INT_MIN;
			break;
		case MORPH_TUI_KEY_END:
			scroll = INT_MAX;
			break;
		case MORPH_TUI_KEY_WHEEL_UP:
			scroll = -3;
			break;
		case MORPH_TUI_KEY_WHEEL_DOWN:
			scroll = 3;
			break;
		case MORPH_TUI_KEY_FOCUS_IN:
		case MORPH_TUI_KEY_FOCUS_OUT:
			cli_recap_focus(page->ctx, event->key == MORPH_TUI_KEY_FOCUS_IN,
					cli_recap_now());
			break;
		default:
			break;
		}
	}
	if (scroll) {
		if (scroll == INT_MIN) {
			page->follow = 0;
			page->offset = 0;
		} else if (scroll == INT_MAX)
			page->follow = 1;
		else {
			page->follow = 0;
			if (scroll < 0)
				page->offset = (size_t)-scroll > page->offset
						       ? 0
						       : page->offset - (size_t)-scroll;
			else
				page->offset += (size_t)scroll;
		}
	}
	if (scroll || event->type == MORPH_TUI_EVENT_WAKE || event->type == MORPH_TUI_EVENT_RESIZE)
		morph_tui_application_invalidate(app, MORPH_TUI_INVALIDATE_DISPLAY);
	return 1;
}

static int cli_turn_details_appear(void *data, struct morph_tui_application *app)
{
	struct cli_turn_details *page = data;

	(void)app;
	page->visible = 1;
	morph_tui_frame_invalidate(&page->frame);
	return 0;
}

static void cli_turn_details_disappear(void *data, struct morph_tui_application *app)
{
	(void)app;
	((struct cli_turn_details *)data)->visible = 0;
}

static void cli_turn_details_complete(void *data, int result)
{
	(void)data;
	(void)result;
}

static void cli_turn_details_close(void *data, int result)
{
	struct cli_turn_details *page = data;

	page->open = 0;
	page->visible = 0;
	cli_application_panel_complete(page->ctx, page, cli_turn_details_complete, result);
}

void cli_turn_details_toggle(struct cli_application *ctx)
{
	struct morph_tui_application *app = cli_application_tui_borrow(ctx);

	if (!app || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	if (cli_turn_details_is_open(ctx)) {
		(void)morph_tui_application_dismiss_controller(app, 0);
		return;
	}
	if (cli_turn_details_prepare(ctx))
		return;
	if (!ctx->transcript && cli_transcript_model_create(&ctx->transcript))
		return;
	static const struct morph_tui_controller_ops ops = {
		.event = cli_turn_details_event,
		.draw = cli_turn_details_draw,
		.appear = cli_turn_details_appear,
		.disappear = cli_turn_details_disappear,
		.close = cli_turn_details_close,
	};
	struct morph_tui_controller controller = {
		.ops = &ops,
		.data = ctx->details,
		.input_mode = MORPH_TUI_INPUT_KEYS,
		.terminal_flags = MORPH_TUI_TERMINAL_SESSION_ALTERNATE |
				  MORPH_TUI_TERMINAL_SESSION_PASTE |
				  MORPH_TUI_TERMINAL_SESSION_MOUSE,
	};

	ctx->details->follow = 1;
	if (!morph_tui_application_present_controller_take(app, &controller))
		ctx->details->open = 1;
}

void cli_turn_details_cleanup(struct cli_application *ctx)
{
	if (!ctx || !ctx->details)
		return;
	morph_tui_frame_cleanup(&ctx->details->frame);
	free(ctx->details);
	ctx->details = NULL;
}
