#include "sapi/cli/pages/model.h"
#include "nb/err.h"
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/adapters/input/readline.h"
#include "sapi/cli/widgets/composer_panel.h"
#include "nb/cleanup.h"
#include <morph/tui/components.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct cli_model_picker {
	struct cli_application *application;
	int current;
	char model[128];
	struct morph_tui_widget_menu_state menu;
	int selection;
};

static int cli_model_draw(void *opaque, struct cli_composer_panel_frame *frame)
{
	struct cli_model_picker *picker = opaque;
	const char *labels[] = {"Normal", "Ultra"};
	const char *descriptions[] = {
		"Delegate only when explicitly requested.",
		"Proactively collaborate with parallel agents.",
	};
	nb_buf label NB_CLEANUP(nb_buf_cleanup) = {0};
	int selected_index = picker->menu.list.index < 2 ? (int)picker->menu.list.index : 0;
	int budget = frame->rows;
	int header = budget >= 7 ? 2 : 1;
	int footer = budget >= 10 ? 3 : budget >= 4 ? 1 : 0;
	int page = budget - header - footer;
	if (page < 1)
		page = 1;
	if (page > 2)
		page = 2;
	morph_tui_widget_list_state_layout(&picker->menu.list, 2, (size_t)page);
	int rc = (nb_buf_init(&label), nb_buf_reserve(&label, 128) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	(void)cli_composer_panel_line(frame, "  Select Model Mode", CLI_INPUT_BACKGROUND ANSI_BOLD);
	if (header == 2) {
		(void)nb_buf_printf(&label, "  %s", picker->model);
		(void)cli_composer_panel_line(frame, nb_buf_cstr(&label),
					      CLI_INPUT_BACKGROUND ANSI_DIM);
	}
	for (size_t i = picker->menu.list.first;
	     i < 2 && i - picker->menu.list.first < picker->menu.list.page; i++) {
		int selected = picker->menu.list.index == i;
		nb_buf_clear(&label);
		(void)nb_buf_printf(&label, "%s %zu. %s%s", selected ? ">" : " ", i + 1, labels[i],
				    picker->current == (int)i ? " (current)" : "");
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

static int cli_model_event(void *opaque, const struct morph_tui_event *event)
{
	struct cli_model_picker *picker = opaque;
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
			CMD_ERROR(ctx, "model mode change failed: %s",
				  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		}
	}
	free(picker);
}

int cli_model_show(struct cli_application *ctx)
{
	struct cli_model_picker *picker = calloc(1, sizeof(*picker));
	static const struct cli_composer_panel_ops cli_model_ops = {
		.draw = cli_model_draw,
		.update = cli_model_event,
		.complete = cli_model_complete,
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
	int rc = runtime_model_get_ultra(ctx->runtime, &picker->current);
	if (rc == 0) {
		struct session session = {0};
		(void)runtime_session_current(ctx->runtime, &session);
		char *safe NB_CLEANUP(nb_charp_cleanup) =
			morph_terminal_sanitize_dup(session.model, strlen(session.model),
						    MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
		if (!safe)
			rc = NB_ERR_NOMEM;
		else {
			(void)morph_terminal_copy_ellipsized_display_width(
				picker->model, sizeof(picker->model), safe, 48, 0);
			picker->menu.list.index = (size_t)picker->current;
			rc = cli_composer_panel_show(ctx, &options, &cli_model_ops, picker);
		}
	}
	if (rc != 0)
		free(picker);
	return rc;
}
