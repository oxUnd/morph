#include "sapi/cli/pages/rewind.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/widgets/list.h"
#include "util/cleanup.h"
#include "util/json.h"
#include "util/error.h"
#include <morph/tui/components.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cli_rewind_picker {
	struct cli_application *ctx;
	cJSON *listing;
	cJSON *preview;
	struct morph_tui_widget_menu_state menu;
	cli_rewind_apply_fn apply;
	int64_t id;
	int stage;
	enum runtime_rewind_mode mode;
	char error[256];
};

static void cli_rewind_cleanup(struct cli_rewind_picker *picker)
{
	cJSON_Delete(picker->listing);
	cJSON_Delete(picker->preview);
	free(picker);
}

static const char *cli_rewind_prompt(const cJSON *entry)
{
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "prompt"));
	return text ? text : "";
}

static int cli_rewind_available(const struct cli_rewind_picker *picker,
				enum runtime_rewind_mode mode)
{
	if ((mode & RUNTIME_REWIND_CODE) &&
	    (cJSON_IsFalse(cJSON_GetObjectItem(picker->preview, "code_active")) ||
	     cJSON_IsFalse(cJSON_GetObjectItem(picker->preview, "code_restorable"))))
		return 0;
	return !(mode & RUNTIME_REWIND_CONVERSATION) ||
	       !cJSON_IsFalse(cJSON_GetObjectItem(picker->preview, "conversation_restorable"));
}

static const char *cli_rewind_reason(const struct cli_rewind_picker *picker)
{
	const char *reason =
		cJSON_GetStringValue(cJSON_GetObjectItem(picker->preview, "invalid_reason"));
	if (!reason || !*reason)
		reason = cJSON_GetStringValue(cJSON_GetObjectItem(picker->preview, "reason"));
	return reason && *reason ? reason
				 : "Code restore is unavailable; inspect conflicts or choose "
				   "conversation only.";
}

static void cli_rewind_cell(morph_buf_t *frame, const struct morph_tui_screen *screen, int row,
			    const char *text, int selected)
{
	char compact[BUFSIZ];
	cli_list_compact_text(compact, sizeof(compact), text);
	(void)morph_tui_draw_cell_append_aligned(frame, screen->area.width, screen->area.height,
						 row, 1, screen->area.width, compact,
						 selected ? ANSI_CYAN ANSI_BOLD : ANSI_RESET, 0);
}

static int cli_rewind_draw(void *opaque, const struct morph_tui_screen *screen)
{
	struct cli_rewind_picker *picker = opaque;
	morph_buf_t frame MORPH_CLEANUP(morph_buf_cleanup) = {0};
	morph_buf_t label MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int height = screen->area.height;
	int first = picker->stage && height >= 10  ? 5
		    : picker->stage && height >= 8 ? 4
		    : height >= 6		   ? 3
						   : 2;
	int page = height - first;
	if (page < 1)
		page = 1;
	const cJSON *entries = cJSON_GetObjectItemCaseSensitive(picker->listing, "checkpoints");
	int count = picker->stage ? 3 : cJSON_GetArraySize(entries);
	morph_tui_widget_list_state_layout(&picker->menu.list, (size_t)count, (size_t)page);
	int rc = morph_buf_init(&frame, BUFSIZ);
	if (rc == 0)
		rc = morph_buf_init(&label, 256);
	if (rc != 0)
		return rc;
	(void)morph_buf_puts(&frame, "\033[?2026h\033[0m\033[H\033[J");
	cli_rewind_cell(&frame, screen, 1,
			picker->stage && height < 8 ? cli_rewind_prompt(picker->preview)
			: picker->stage		    ? "Rewind · choose what to restore"
						    : "Rewind · choose a turn",
			0);
	if (height >= 6) {
		if (picker->stage) {
			if (height >= 8)
				cli_rewind_cell(&frame, screen, 2,
						cli_rewind_prompt(picker->preview), 0);
			(void)morph_buf_printf(
				&label, "#%lld · %d files · %d conflicts", (long long)picker->id,
				cJSON_GetArraySize(cJSON_GetObjectItem(picker->preview, "files")),
				cJSON_GetArraySize(
					cJSON_GetObjectItem(picker->preview, "conflicts")));
			cli_rewind_cell(
				&frame, screen, height >= 8 ? 3 : 2,
				height < 10 && !cli_rewind_available(picker, RUNTIME_REWIND_CODE)
					? cli_rewind_reason(picker)
					: morph_buf_cstr_borrow(&label),
				0);
			if (height >= 10)
				cli_rewind_cell(&frame, screen, 4,
						*picker->error ? picker->error
						: !cli_rewind_available(picker, RUNTIME_REWIND_CODE)
							? cli_rewind_reason(picker)
							: "Files below will return to their state "
							  "before this turn.",
						0);
		} else {
			cli_rewind_cell(&frame, screen, 2,
					*picker->error ? picker->error
						       : "Each checkpoint precedes its user turn.",
					0);
		}
	}
	const char *modes[] = {"Code and conversation", "Code only", "Conversation only"};
	const enum runtime_rewind_mode mode_values[] = {RUNTIME_REWIND_BOTH, RUNTIME_REWIND_CODE,
							RUNTIME_REWIND_CONVERSATION};
	int row = first;
	for (size_t i = picker->menu.list.first;
	     i < (size_t)count && i - picker->menu.list.first < picker->menu.list.page;
	     i++, row++) {
		morph_buf_reset(&label);
		if (picker->stage)
			(void)morph_buf_printf(&label, "%s %s%s",
					       picker->menu.list.index == i ? ">" : " ", modes[i],
					       cli_rewind_available(picker, mode_values[i])
						       ? ""
						       : " (unavailable)");
		else {
			const cJSON *entry = cJSON_GetArrayItem(entries, (int)i);
			(void)morph_buf_printf(
				&label, "%s #%lld  %s",
				picker->menu.list.index == i ? ">" : " ",
				(long long)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "id")),
				cli_rewind_prompt(entry));
		}
		cli_rewind_cell(&frame, screen, row, morph_buf_cstr_borrow(&label),
				picker->menu.list.index == i);
	}
	if (picker->stage) {
		const cJSON *file;
		cJSON_ArrayForEach (file, cJSON_GetObjectItem(picker->preview, "files")) {
			if (row >= height)
				break;
			const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(file, "path"));
			cli_rewind_cell(&frame, screen, row++, path ? path : "", 0);
		}
	}
	if (height >= 3)
		cli_rewind_cell(&frame, screen, height,
				picker->stage
					? "Up/Down Select · Enter Restore · Left Back · Esc Cancel"
					: "Up/Down Select · Enter Preview · Esc Cancel",
				0);
	(void)morph_buf_puts(&frame, "\033[0m\033[?2026l");
	if (label.failed)
		return label.failed;
	return morph_tui_draw_write(stdout, &frame);
}

static int cli_rewind_update(void *opaque, const struct morph_tui_event *event)
{
	struct cli_rewind_picker *picker = opaque;
	if (event->type != MORPH_TUI_EVENT_KEY || event->pasting)
		return MORPH_TUI_SCREEN_CONTINUE;
	if (event->key == 27)
		return MORPH_TUI_SCREEN_CLOSE;
	int action = event->key == MORPH_TUI_KEY_LEFT
			     ? MORPH_TUI_WIDGET_BACK
			     : morph_tui_widget_menu_state_update(&picker->menu, event->key);
	if (action == MORPH_TUI_WIDGET_CANCEL || event->key == 'q')
		return MORPH_TUI_SCREEN_CLOSE;
	if (action == MORPH_TUI_WIDGET_BACK) {
		if (!picker->stage)
			return MORPH_TUI_SCREEN_CLOSE;
		picker->stage = 0;
		picker->menu.list.index = 0;
		picker->error[0] = '\0';
		morph_tui_widget_list_state_layout(&picker->menu.list,
						   (size_t)cJSON_GetArraySize(cJSON_GetObjectItem(
							   picker->listing, "checkpoints")),
						   picker->menu.list.page);
		return MORPH_TUI_SCREEN_REDRAW;
	}
	if (action == MORPH_TUI_WIDGET_SUBMIT) {
		if (picker->stage) {
			const enum runtime_rewind_mode modes[] = {RUNTIME_REWIND_BOTH,
								  RUNTIME_REWIND_CODE,
								  RUNTIME_REWIND_CONVERSATION};
			if (!cli_rewind_available(picker, modes[picker->menu.list.index])) {
				snprintf(picker->error, sizeof(picker->error),
					 "That restore mode is unavailable.");
				return MORPH_TUI_SCREEN_REDRAW;
			}
			picker->mode = modes[picker->menu.list.index];
			return MORPH_TUI_SCREEN_CLOSE;
		}
		const cJSON *entry =
			cJSON_GetArrayItem(cJSON_GetObjectItem(picker->listing, "checkpoints"),
					   (int)picker->menu.list.index);
		picker->id = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "id"));
		morph_json_cleanup(&picker->preview);
		int rc = runtime_checkpoint_preview_copy(picker->ctx->runtime, picker->id,
							 &picker->preview);
		if (rc)
			snprintf(picker->error, sizeof(picker->error), "Preview failed: %s",
				 morph_strerror(rc));
		else {
			picker->error[0] = '\0';
			picker->stage = 1;
			picker->menu.list.index =
				cli_rewind_available(picker, RUNTIME_REWIND_BOTH)	    ? 0
				: cli_rewind_available(picker, RUNTIME_REWIND_CONVERSATION) ? 2
											    : 1;
			picker->menu.list.first = 0;
			morph_tui_widget_list_state_layout(&picker->menu.list, 3,
							   picker->menu.list.page);
		}
		return MORPH_TUI_SCREEN_REDRAW;
	}
	return action == MORPH_TUI_WIDGET_CHANGED ? MORPH_TUI_SCREEN_REDRAW
						  : MORPH_TUI_SCREEN_CONTINUE;
}

static void cli_rewind_close(void *opaque, int result)
{
	struct cli_rewind_picker *picker = opaque;
	if (!result && picker->mode)
		(void)picker->apply(picker->ctx, picker->id, picker->mode);
	cli_rewind_cleanup(picker);
}

int cli_rewind_show_take(struct cli_application *ctx, cJSON **listing, cli_rewind_apply_fn apply)
{
	if (!ctx || !listing || !*listing || !apply ||
	    cJSON_GetArraySize(cJSON_GetObjectItem(*listing, "checkpoints")) <= 0)
		return -EINVAL;
	struct cli_rewind_picker *picker = calloc(1, sizeof(*picker));
	if (!picker)
		return -ENOMEM;
	picker->ctx = ctx;
	picker->listing = *listing;
	picker->apply = apply;
	picker->menu.list.flags = MORPH_TUI_WIDGET_LIST_VIM | MORPH_TUI_WIDGET_LIST_PAGED;
	static const struct morph_tui_screen_ops ops = {
		.update = cli_rewind_update,
		.draw = cli_rewind_draw,
	};
	int rc;
	if (cli_application_tui_borrow(ctx))
		rc = cli_application_present_screen(ctx, &ops, picker, cli_rewind_close);
	else {
		struct morph_tui_screen screen = {0};
		rc = morph_tui_screen_open(&screen, stdin, stdout,
					   MORPH_TUI_TERMINAL_SESSION_ALTERNATE |
						   MORPH_TUI_TERMINAL_SESSION_PASTE);
		if (rc == 0)
			rc = morph_tui_screen_run(&screen, &ops, picker);
		int leave_rc = morph_tui_screen_close(&screen);
		if (rc == 0)
			rc = leave_rc;
		if (rc == 0) {
			*listing = NULL;
			cli_rewind_close(picker, 0);
			return 0;
		}
	}
	if (rc == 0)
		*listing = NULL;
	else {
		picker->listing = NULL;
		cli_rewind_cleanup(picker);
	}
	return rc;
}
