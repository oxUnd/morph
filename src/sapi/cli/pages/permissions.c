#include "sapi/cli/pages/permissions.h"
#include "sapi/cli/application.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/output.h"
#include <morph/tui/components.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct cli_permissions_picker {
	struct cli_application *application;
	struct permission_policy current;
	struct morph_tui_widget_menu_state menu;
	int selection;
};

static const char *cli_permissions_preset(int index)
{
	const char *names[] = {"ask", "auto", "full", "read_only"};
	return index >= 0 && index < 4 ? names[index] : "custom";
}

static int cli_permissions_draw(void *opaque, const struct morph_tui_screen *screen)
{
	struct cli_permissions_picker *picker = opaque;
	const char *labels[] = {"Ask for approval", "Approve for me", "Full Access", "Read Only"};
	morph_buf_t frame = {0};
	int rc = morph_buf_init(&frame, BUFSIZ);

	morph_tui_widget_list_state_layout(&picker->menu.list, 4, 4);
	if (rc == 0)
		rc = morph_buf_printf(&frame,
			"\033[0m\033[2J\033[H\033[1mUpdate Model Permissions\033[0m\r\n\r\n"
			"Current: %s\r\n\r\n", runtime_permission_preset_name(&picker->current));
	for (int i = 0; rc == 0 && i < 4; i++) {
		int selected = picker->menu.list.index == (size_t)i;

		rc = morph_buf_printf(&frame, "%s%s %d. %s\033[0m\033[K\r\n",
			selected ? "\033[44;97m" : "", selected ? ">" : " ", i + 1, labels[i]);
	}
	if (rc == 0 && screen->area.height > 10)
		rc = morph_buf_puts(&frame, "\r\nUp/Down Select  Enter Apply  Esc Cancel\r\n");
	if (rc == 0)
		rc = morph_tui_draw_write(stdout, &frame);
	morph_buf_cleanup(&frame);
	return rc;
}

static int cli_permissions_update(void *opaque, const struct morph_tui_event *event)
{
	struct cli_permissions_picker *picker = opaque;
	int result;

	if (event->type != MORPH_TUI_EVENT_KEY || event->pasting)
		return MORPH_TUI_SCREEN_CONTINUE;
	result = morph_tui_widget_menu_state_update(&picker->menu, event->key);
	if (result == MORPH_TUI_WIDGET_SUBMIT) {
		picker->selection = (int)picker->menu.list.index;
		return MORPH_TUI_SCREEN_CLOSE;
	}
	if (result == MORPH_TUI_WIDGET_BACK || result == MORPH_TUI_WIDGET_CANCEL)
		return MORPH_TUI_SCREEN_CLOSE;
	return result == MORPH_TUI_WIDGET_CHANGED ? MORPH_TUI_SCREEN_REDRAW :
		MORPH_TUI_SCREEN_CONTINUE;
}

static void cli_permissions_close(void *opaque, int result)
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

int cli_permissions_show(struct cli_application *ctx)
{
	struct cli_permissions_picker *picker = calloc(1, sizeof(*picker));
	const struct morph_tui_screen_ops ops = {
		.draw = cli_permissions_draw, .update = cli_permissions_update};
	int rc;

	if (!picker)
		return -ENOMEM;
	picker->application = ctx;
	picker->selection = -1;
	rc = runtime_permission_get_policy(ctx->runtime, &picker->current);
	if (rc == 0) {
		for (int i = 0; i < 4; i++) {
			if (!strcmp(runtime_permission_preset_name(&picker->current),
			    cli_permissions_preset(i)))
				picker->menu.list.index = (size_t)i;
		}
		rc = cli_application_present_screen(ctx, &ops, picker, cli_permissions_close);
	}
	if (rc != 0)
		free(picker);
	return rc;
}
