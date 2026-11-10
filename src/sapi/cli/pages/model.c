#include "sapi/cli/pages/model.h"
#include "sapi/cli/application.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/adapters/input/readline.h"
#include "util/cleanup.h"
#include <morph/tui/components.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Keep physical lines stable across common terminal widths while open. */
#define CLI_MODEL_CONTENT_WIDTH 58u

struct cli_model_picker {
	struct cli_application *application;
	int current;
	char model[128];
	struct morph_tui_widget_menu_state menu;
	int selection;
	int painted_rows;
};

static void cli_model_line(struct cli_model_picker *picker, morph_buf_t *frame, const char *text,
			   const char *style, size_t width)
{
	const char *end = utf8_advance_display_width_borrow(text, width);
	int color = cli_output_color_enabled(picker->application);

	(void)morph_buf_puts(frame, color ? style : "");
	(void)morph_buf_append(frame, text, (size_t)(end - text));
	(void)morph_buf_puts(frame, "\033[K\033[0m\r\n");
}

static int cli_model_draw(void *opaque, const struct morph_tui_screen *screen)
{
	struct cli_model_picker *picker = opaque;
	const char *labels[] = {"Normal", "Ultra"};
	const char *descriptions[] = {
		"Delegate only when explicitly requested.",
		"Proactively collaborate with parallel agents.",
	};
	morph_buf_t frame MORPH_CLEANUP(morph_buf_cleanup) = {0};
	morph_buf_t label MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int selected_index = picker->menu.list.index < 2 ? (int)picker->menu.list.index : 0;
	int budget = screen->area.height > 1 ? screen->area.height - 1 : 1;
	int header = budget >= 7 ? 3 : 1;
	int footer = budget >= 10 ? 3 : budget >= 4 ? 1 : 0;
	int page = budget - header - footer;
	int rows = 0;
	size_t width = screen->area.width > 1 ? (size_t)screen->area.width - 1 : 0;
	int rc = morph_buf_init(&frame, BUFSIZ);

	if (width > CLI_MODEL_CONTENT_WIDTH)
		width = CLI_MODEL_CONTENT_WIDTH;
	if (page < 1)
		page = 1;
	if (page > 2)
		page = 2;
	morph_tui_widget_list_state_layout(&picker->menu.list, 2, (size_t)page);
	if (!rc)
		rc = morph_buf_init(&label, 128);
	if (rc) {
		return rc;
	}
	(void)morph_buf_puts(&frame, "\033[?2026h\033[0m\r");
	if (picker->painted_rows)
		(void)morph_buf_printf(&frame, "\033[%dA\033[J", picker->painted_rows);
	if (header == 3) {
		cli_model_line(picker, &frame, "", ANSI_RESET, width);
		rows++;
	}
	cli_model_line(picker, &frame, "  Select Model Mode", CLI_INPUT_BACKGROUND ANSI_BOLD,
		       width);
	rows++;
	if (header == 3) {
		morph_buf_reset(&label);
		(void)morph_buf_printf(&label, "  %s", picker->model);
		cli_model_line(picker, &frame, morph_buf_cstr_borrow(&label),
			       CLI_INPUT_BACKGROUND ANSI_DIM, width);
		rows++;
	}
	for (size_t i = picker->menu.list.first;
	     i < 2 && i - picker->menu.list.first < picker->menu.list.page; i++) {
		int selected = picker->menu.list.index == i;

		morph_buf_reset(&label);
		(void)morph_buf_printf(&label, "%s %zu. %s%s", selected ? ">" : " ", i + 1,
				       labels[i], picker->current == (int)i ? " (current)" : "");
		cli_model_line(picker, &frame, morph_buf_cstr_borrow(&label),
			       selected ? "\033[44;97m" : CLI_INPUT_BACKGROUND, width);
		rows++;
	}
	if (footer == 3) {
		cli_model_line(picker, &frame, "", CLI_INPUT_BACKGROUND, width);
		cli_model_line(picker, &frame, descriptions[selected_index],
			       CLI_INPUT_BACKGROUND ANSI_DIM, width);
		rows += 2;
	}
	if (footer) {
		cli_model_line(picker, &frame, "  Up/Down Select  Enter Apply  Esc Cancel",
			       CLI_INPUT_BACKGROUND ANSI_DIM, width);
		rows++;
	}
	if (label.failed)
		frame.failed = label.failed;
	(void)morph_buf_puts(&frame, "\033[?2026l");
	rc = morph_tui_draw_write(stdout, &frame);
	if (!rc)
		picker->painted_rows = rows;
	return rc;
}

static int cli_model_event(void *opaque, struct morph_tui_application *app,
			   const struct morph_tui_event *event)
{
	struct cli_model_picker *picker = opaque;
	int result;

	if (event->type != MORPH_TUI_EVENT_KEY || event->pasting)
		return 0;
	result = morph_tui_widget_menu_state_update(&picker->menu, event->key);
	if (result == MORPH_TUI_WIDGET_SUBMIT)
		picker->selection = (int)picker->menu.list.index;
	if (result == MORPH_TUI_WIDGET_SUBMIT || result == MORPH_TUI_WIDGET_BACK ||
	    result == MORPH_TUI_WIDGET_CANCEL)
		return morph_tui_application_dismiss_controller(app, 0);
	if (result == MORPH_TUI_WIDGET_CHANGED)
		morph_tui_application_invalidate(app, MORPH_TUI_INVALIDATE_DISPLAY);
	return 1;
}

static void cli_model_disappear(void *opaque, struct morph_tui_application *app)
{
	struct cli_model_picker *picker = opaque;

	(void)app;
	if (picker->painted_rows) {
		fprintf(stdout, "\033[?2026h\033[0m\r\033[%dA\033[J\033[?2026l",
			picker->painted_rows);
		fflush(stdout);
		picker->painted_rows = 0;
	}
}

static void cli_model_complete(void *opaque, int result)
{
	struct cli_model_picker *picker = opaque;
	struct cli_application *ctx = picker->application;

	if (result == 0 && picker->selection >= 0 && picker->selection < 2) {
		int rc = runtime_model_set_ultra(ctx->runtime, picker->selection);
		if (rc == 0) {
			cli_readline_sync_mode(ctx, 1);
			CMD_OK(ctx, "model mode: %s", picker->selection ? "ultra" : "normal");
		} else {
			CMD_ERROR(ctx, "model mode change failed: %s", morph_strerror(rc));
		}
	}
	free(picker);
}

static void cli_model_close(void *opaque, int result)
{
	struct cli_model_picker *picker = opaque;

	cli_application_panel_complete(picker->application, picker, cli_model_complete, result);
}

int cli_model_show(struct cli_application *ctx)
{
	struct cli_model_picker *picker = calloc(1, sizeof(*picker));
	static const struct morph_tui_controller_ops ops = {
		.draw = cli_model_draw,
		.event = cli_model_event,
		.disappear = cli_model_disappear,
		.close = cli_model_close,
	};
	struct morph_tui_controller controller = {
		.ops = &ops,
		.data = picker,
		.input_mode = MORPH_TUI_INPUT_KEYS,
		.terminal_flags = MORPH_TUI_TERMINAL_SESSION_PASTE,
		.tick_ms = 100,
	};
	int rc;

	if (!picker)
		return -ENOMEM;
	picker->application = ctx;
	picker->selection = -1;
	picker->menu.list.flags = MORPH_TUI_WIDGET_LIST_WRAP | MORPH_TUI_WIDGET_LIST_DIGITS;
	rc = runtime_model_get_ultra(ctx->runtime, &picker->current);
	if (rc == 0) {
		struct session session = {0};
		(void)runtime_session_current(ctx->runtime, &session);
		char *safe MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(
			session.model, strlen(session.model), UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
		if (!safe)
			rc = -ENOMEM;
		else {
			(void)utf8_copy_ellipsized_display_width(
				picker->model, sizeof(picker->model), safe, 48, 0);
			picker->menu.list.index = (size_t)picker->current;
			rc = morph_tui_application_present_controller_take(
				cli_application_tui_borrow(ctx), &controller);
		}
	}
	if (rc != 0)
		free(picker);
	return rc;
}
