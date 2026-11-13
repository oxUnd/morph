#include "sapi/cli/pages/permissions.h"
#include "nb/err.h"
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/widgets/composer_panel.h"
#include "nb/cleanup.h"
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

static int cli_permissions_draw(void *opaque, struct cli_composer_panel_frame *frame)
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
	nb_buf label NB_CLEANUP(nb_buf_cleanup) = {0};
	int selected_index = picker->menu.list.index < 4 ? (int)picker->menu.list.index : 0;
	int budget = frame->rows;
	int header = budget >= 7 ? 2 : 1;
	int footer = budget >= 10 ? 3 : budget >= 4 ? 1 : 0;
	int custom = !strcmp(current, "custom") && budget >= 8;
	int page = budget - header - footer - custom;
	if (page < 1)
		page = 1;
	if (page > 4)
		page = 4;
	morph_tui_widget_list_state_layout(&picker->menu.list, 4, (size_t)page);
	int rc = (nb_buf_init(&label), nb_buf_reserve(&label, 128) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	(void)cli_composer_panel_line(frame, "  Update Model Permissions",
				      CLI_INPUT_BACKGROUND ANSI_BOLD);
	if (header == 2)
		(void)cli_composer_panel_line(frame, "", CLI_INPUT_BACKGROUND);
	if (custom) {
		(void)nb_buf_printf(&label, "  Custom: %s / %s / %s",
				    permission_access_name(picker->current.access),
				    permission_approval_name(picker->current.approval),
				    permission_reviewer_name(picker->current.reviewer));
		(void)cli_composer_panel_line(frame, nb_buf_cstr(&label),
					      CLI_INPUT_BACKGROUND ANSI_DIM);
	}
	for (size_t i = picker->menu.list.first;
	     i < 4 && i - picker->menu.list.first < picker->menu.list.page; i++) {
		int selected = picker->menu.list.index == i;
		nb_buf_clear(&label);
		(void)nb_buf_printf(&label, "%s %zu. %s%s", selected ? ">" : " ", i + 1, labels[i],
				    !strcmp(current, cli_permissions_preset((int)i)) ? " (current)"
										     : "");
		(void)cli_composer_panel_line(frame, nb_buf_cstr(&label),
					      selected ? "\033[44;97m" : CLI_INPUT_BACKGROUND);
	}
	if (footer == 3) {
		(void)cli_composer_panel_line(frame, "", CLI_INPUT_BACKGROUND);
		(void)cli_composer_panel_line(frame, descriptions[selected_index],
					      CLI_INPUT_BACKGROUND ANSI_DIM);
	}
	if (footer)
		(void)cli_composer_panel_line(frame, "  Up/Down Select  Enter Apply  Esc Cancel",
					      CLI_INPUT_BACKGROUND ANSI_DIM);
	return nb_buf_status(&label);
}

static int cli_permissions_event(void *opaque, const struct morph_tui_event *event)
{
	struct cli_permissions_picker *picker = opaque;
	if (event->type != MORPH_TUI_EVENT_KEY || event->pasting)
		return MORPH_TUI_SCREEN_CONTINUE;
	int result = morph_tui_widget_menu_state_update(&picker->menu, event->key);
	if (result == MORPH_TUI_WIDGET_SUBMIT)
		picker->selection = (int)picker->menu.list.index;
	if (result == MORPH_TUI_WIDGET_SUBMIT || result == MORPH_TUI_WIDGET_BACK ||
	    result == MORPH_TUI_WIDGET_CANCEL)
		return MORPH_TUI_SCREEN_CLOSE;
	return result == MORPH_TUI_WIDGET_CHANGED ? MORPH_TUI_SCREEN_REDRAW
						  : MORPH_TUI_SCREEN_CONTINUE;
}

static void cli_permissions_complete(void *opaque, int result)
{
	struct cli_permissions_picker *picker = opaque;
	struct cli_application *ctx = picker->application;
	if (result == 0 && picker->selection >= 0 && picker->selection < 4) {
		struct permission_policy policy;
		int rc = runtime_permission_preset(cli_permissions_preset(picker->selection),
						   &policy);
		if (rc == 0)
			rc = runtime_permission_set_policy(ctx->runtime, &policy);
		if (rc == 0)
			CMD_OK(ctx, "permissions: %s", runtime_permission_preset_name(&policy));
		else
			CMD_ERROR(ctx, "permission change failed: %s",
				  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	}
	free(picker);
}

int cli_permissions_show(struct cli_application *ctx)
{
	struct cli_permissions_picker *picker = calloc(1, sizeof(*picker));
	static const struct cli_composer_panel_ops cli_permissions_ops = {
		.draw = cli_permissions_draw,
		.update = cli_permissions_event,
		.complete = cli_permissions_complete,
	};
	const struct cli_composer_panel_options options = {
		.max_rows = 10,
		.tick_ms = 100,
	};
	if (!picker)
		return NB_ERR_NOMEM;
	picker->application = ctx;
	picker->selection = -1;
	picker->menu.list.flags = MORPH_TUI_WIDGET_LIST_WRAP | MORPH_TUI_WIDGET_LIST_DIGITS;
	int rc = runtime_permission_get_policy(ctx->runtime, &picker->current);
	if (rc == 0) {
		for (int i = 0; i < 4; i++) {
			if (!strcmp(runtime_permission_preset_name(&picker->current),
				    cli_permissions_preset(i)))
				picker->menu.list.index = (size_t)i;
		}
		rc = cli_composer_panel_show(ctx, &options, &cli_permissions_ops, picker);
	}
	if (rc != 0)
		free(picker);
	return rc;
}
