#include "sapi/cli/pages/permissions.h"
#include "sapi/cli/application.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/output.h"
#include <morph/tui/components.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Keep physical lines stable across common terminal widths while open. */
#define CLI_PERMISSIONS_CONTENT_WIDTH 58u

struct cli_permissions_picker {
	struct cli_application *application;
	struct permission_policy current;
	struct morph_tui_widget_menu_state menu;
	int selection;
	int painted_rows;
};

static const char *cli_permissions_preset(int index)
{
	const char *names[] = {"ask", "auto", "full", "read_only"};
	return index >= 0 && index < 4 ? names[index] : "custom";
}

static void cli_permissions_line(struct cli_permissions_picker *picker, morph_buf_t *frame,
				const char *text, const char *style, size_t width)
{
	const char *end = utf8_advance_display_width(text, width);
	int color = cli_output_color_enabled(picker->application);

	(void)morph_buf_puts(frame, color ? style : "");
	(void)morph_buf_append(frame, text, (size_t)(end - text));
	(void)morph_buf_puts(frame, "\033[K\033[0m\r\n");
}

static int cli_permissions_draw(void *opaque, const struct morph_tui_screen *screen)
{
	struct cli_permissions_picker *picker = opaque;
	const char *labels[] = {"Ask for approval", "Approve for me", "Full Access", "Read Only"};
	const char *descriptions[] = {
		"Workspace edits; ask before extra access.",
		"Model reviews requests for extra access.",
		"All files and network, without approval.",
		"Read files; ask before edits or network.",
	};
	const char *current = runtime_permission_preset_name(&picker->current);
	morph_buf_t frame = {0}, label = {0};
	int budget = screen->area.height > 1 ? screen->area.height - 1 : 1;
	int header = budget >= 7 ? 3 : 1;
	int footer = budget >= 10 ? 3 : budget >= 4 ? 1 : 0;
	int custom = !strcmp(current, "custom") && budget >= 8;
	int page = budget - header - footer - custom;
	int rows = 0;
	size_t width = screen->area.width > 1 ? (size_t)screen->area.width - 1 : 0;
	int rc = morph_buf_init(&frame, BUFSIZ);

	if (width > CLI_PERMISSIONS_CONTENT_WIDTH)
		width = CLI_PERMISSIONS_CONTENT_WIDTH;
	if (page < 1)
		page = 1;
	if (page > 4)
		page = 4;
	morph_tui_widget_list_state_layout(&picker->menu.list, 4, (size_t)page);
	if (!rc)
		rc = morph_buf_init(&label, 128);
	if (rc) {
		morph_buf_cleanup(&frame);
		return rc;
	}
	(void)morph_buf_puts(&frame, "\033[?2026h\033[0m\r");
	if (picker->painted_rows)
		(void)morph_buf_printf(&frame, "\033[%dA\033[J", picker->painted_rows);
	if (header == 3) {
		cli_permissions_line(picker, &frame, "", ANSI_RESET, width);
		rows++;
	}
	cli_permissions_line(picker, &frame, "  Update Model Permissions",
		CLI_INPUT_BACKGROUND ANSI_BOLD, width);
	rows++;
	if (header == 3) {
		cli_permissions_line(picker, &frame, "", CLI_INPUT_BACKGROUND, width);
		rows++;
	}
	if (custom) {
		(void)morph_buf_printf(&label, "  Custom: %s / %s / %s",
			permission_access_name(picker->current.access),
			permission_approval_name(picker->current.approval),
			permission_reviewer_name(picker->current.reviewer));
		cli_permissions_line(picker, &frame, morph_buf_cstr(&label),
			CLI_INPUT_BACKGROUND ANSI_DIM, width);
		rows++;
	}
	for (size_t i = picker->menu.list.first; i < 4 &&
	     i - picker->menu.list.first < picker->menu.list.page; i++) {
		int selected = picker->menu.list.index == i;

		morph_buf_reset(&label);
		(void)morph_buf_printf(&label, "%s %zu. %s%s", selected ? ">" : " ", i + 1,
			labels[i], !strcmp(current, cli_permissions_preset((int)i)) ?
				" (current)" : "");
		cli_permissions_line(picker, &frame, morph_buf_cstr(&label),
			selected ? "\033[44;97m" : CLI_INPUT_BACKGROUND, width);
		rows++;
	}
	if (footer == 3) {
		cli_permissions_line(picker, &frame, "", CLI_INPUT_BACKGROUND, width);
		cli_permissions_line(picker, &frame, descriptions[picker->menu.list.index],
			CLI_INPUT_BACKGROUND ANSI_DIM, width);
		rows += 2;
	}
	if (footer) {
		cli_permissions_line(picker, &frame, "  Up/Down Select  Enter Apply  Esc Cancel",
			CLI_INPUT_BACKGROUND ANSI_DIM, width);
		rows++;
	}
	if (label.failed)
		frame.failed = label.failed;
	(void)morph_buf_puts(&frame, "\033[?2026l");
	rc = morph_tui_draw_write(stdout, &frame);
	if (!rc)
		picker->painted_rows = rows;
	morph_buf_cleanup(&label);
	morph_buf_cleanup(&frame);
	return rc;
}

static int cli_permissions_event(void *opaque, struct morph_tui_application *app,
				 const struct morph_tui_event *event)
{
	struct cli_permissions_picker *picker = opaque;
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

static void cli_permissions_disappear(void *opaque, struct morph_tui_application *app)
{
	struct cli_permissions_picker *picker = opaque;

	(void)app;
	if (picker->painted_rows) {
		fprintf(stdout, "\033[?2026h\033[0m\r\033[%dA\033[J\033[?2026l",
			picker->painted_rows);
		fflush(stdout);
		picker->painted_rows = 0;
	}
}

static void cli_permissions_complete(void *opaque, int result)
{
	struct cli_permissions_picker *picker = opaque;
	struct cli_application *ctx = picker->application;

	if (result == 0 && picker->selection >= 0) {
		struct permission_policy policy;
		int rc = runtime_permission_preset(
			cli_permissions_preset(picker->selection), &policy);

		if (rc == 0)
			rc = runtime_permission_set_policy(ctx->runtime, &policy);
		if (rc == 0)
			CMD_OK(ctx, "permissions: %s", runtime_permission_preset_name(&policy));
		else
			CMD_ERROR(ctx, "permission change failed: %s", morph_strerror(rc));
	}
	free(picker);
}

static void cli_permissions_close(void *opaque, int result)
{
	struct cli_permissions_picker *picker = opaque;

	cli_application_panel_complete(picker->application, picker,
		cli_permissions_complete, result);
}

int cli_permissions_show(struct cli_application *ctx)
{
	struct cli_permissions_picker *picker = calloc(1, sizeof(*picker));
	static const struct morph_tui_controller_ops ops = {
		.draw = cli_permissions_draw,
		.event = cli_permissions_event,
		.disappear = cli_permissions_disappear,
		.close = cli_permissions_close,
	};
	struct morph_tui_controller controller = {
		.ops = &ops, .data = picker,
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
	rc = runtime_permission_get_policy(ctx->runtime, &picker->current);
	if (rc == 0) {
		for (int i = 0; i < 4; i++) {
			if (!strcmp(runtime_permission_preset_name(&picker->current),
			    cli_permissions_preset(i)))
				picker->menu.list.index = (size_t)i;
		}
		rc = morph_tui_application_present_controller(
			cli_application_tui(ctx), &controller);
	}
	if (rc != 0)
		free(picker);
	return rc;
}
