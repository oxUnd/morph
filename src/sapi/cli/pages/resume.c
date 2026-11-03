#include "sapi/cli/pages/resume.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/widgets/list.h"
#include "sapi/cli/pages/setup/controls.h"
#include "sapi/cli/application.h"
#include "sapi/cli/services/session_history.h"
#include <morph/tui/components.h>

static int cli_resume_append_token_number(morph_buf_t *buf, int64_t tokens)
{
	char raw[32];
	const char *digits;
	size_t length;
	int rc;

	snprintf(raw, sizeof(raw), "%lld", (long long)tokens);
	digits = raw;
	if (*digits == '-') {
		rc = morph_buf_putc(buf, *digits++);
		if (rc != 0)
			return rc;
	}
	length = strlen(digits);
	for (size_t i = 0; i < length; i++) {
		if (i > 0 && (length - i) % 3 == 0) {
			rc = morph_buf_putc(buf, ',');
			if (rc != 0)
				return rc;
		}
		rc = morph_buf_putc(buf, digits[i]);
		if (rc != 0)
			return rc;
	}
	return 0;
}

static int cli_resume_append_token_count(morph_buf_t *buf, int64_t tokens)
{
	int rc = cli_resume_append_token_number(buf, tokens);

	return rc == 0 ? morph_buf_puts(buf, " tokens") : rc;
}

static int cli_resume_append_session_time(morph_buf_t *buf, int64_t updated_at, int64_t created_at)
{
	char formatted[16];
	struct tm local;
	time_t timestamp;

	timestamp = (time_t)(updated_at > 0 ? updated_at : created_at);
	if (timestamp <= 0 || !localtime_r(&timestamp, &local) ||
	    strftime(formatted, sizeof(formatted), "%m-%d %H:%M", &local) == 0)
		return morph_buf_puts(buf, " · -- --:--");
	return morph_buf_printf(buf, " · %s", formatted);
}

#define CLI_RESUME_PAGE_SIZE 10

static int cli_resume_page(struct cli_application *ctx, struct session *list, int count, int page,
			   int64_t current_id, int page_size)
{
	int start = page * page_size;
	int end = count - start > page_size ? start + page_size : count;

	CMD_HEADER(ctx, "resume · page %d/%d · %d sessions", page + 1, (count - 1) / page_size + 1,
		   count);
	for (int i = start; i < end; i++) {
		char number[16];
		morph_buf_t metadata;
		int rc = morph_buf_init(&metadata, 64);

		if (rc != 0)
			return rc;
		rc = morph_buf_printf(&metadata, "%s · ", list[i].display_id);
		if (rc == 0)
			rc = cli_resume_append_token_count(&metadata, list[i].token_used);
		if (rc == 0)
			rc = cli_resume_append_session_time(&metadata, list[i].updated_at,
							    list[i].created_at);
		if (rc == 0) {
			snprintf(number, sizeof(number), "%d", i - start + 1);
			cli_list_row(ctx, number, list[i].name, morph_buf_cstr(&metadata),
				     list[i].id == current_id, i == end - 1, cli_list_columns());
		}
		morph_buf_cleanup(&metadata);
		if (rc != 0)
			return rc;
	}
	return 0;
}

#define CLI_RESUME_NORMAL "\033[0;38;2;225;229;234;48;2;16;18;20m"
#define CLI_RESUME_MUTED "\033[0;38;2;142;150;161;48;2;16;18;20m"
#define CLI_RESUME_ACCENT "\033[0;38;2;71;211;230;48;2;16;18;20m"
#define CLI_RESUME_RULE "\033[0;38;2;48;57;65;48;2;16;18;20m"
#define CLI_RESUME_SELECTED "\033[0;38;2;239;244;248;48;2;29;48;56m"
#define CLI_RESUME_SELECTED_META "\033[0;38;2;180;194;203;48;2;29;48;56m"
#define CLI_RESUME_BAR "\033[0;38;2;71;211;230;48;2;29;48;56m"

struct cli_resume_layout {
	int rows;
	int columns;
	int first_row;
	int page_size;
	int width;
	int name_width;
	int id_column;
	int tokens_column;
	int updated_column;
};

static struct cli_resume_layout cli_resume_dimensions(const struct morph_tui_screen *screen)
{
	struct cli_resume_layout layout = {.rows = screen->area.height,
					   .columns = screen->area.width};
	struct morph_tui_terminal_rect body = morph_tui_terminal_rect_inset(screen->area, 2, 0);

	layout.width = body.width > 0 ? body.width : 1;
	layout.first_row = layout.rows >= 14 ? 8 : layout.rows >= 6 ? 3 : layout.rows >= 3 ? 2 : 1;
	layout.page_size = layout.rows >= 14  ? layout.rows - 12
			   : layout.rows >= 6 ? layout.rows - 5
			   : layout.rows >= 3 ? layout.rows - 2
					      : 1;
	layout.name_width = layout.width;
	if (layout.width >= 48) {
		layout.updated_column = 3 + layout.width - 14;
		layout.name_width -= 17;
	}
	if (layout.width >= 68) {
		layout.tokens_column = layout.updated_column - 15;
		layout.name_width -= 15;
	}
	if (layout.width >= 88) {
		layout.id_column = layout.tokens_column - 11;
		layout.name_width -= 11;
	}
	return layout;
}

/* All data passes through the single-line sanitizer before raw frame output. */
static void cli_resume_cell(morph_buf_t *frame, const struct cli_resume_layout *layout, int row,
			    int column, int width, const char *text, const char *style, int right)
{
	char compact[BUFSIZ];

	cli_list_compact_text(compact, sizeof(compact), text);
	(void)morph_tui_draw_cell_append_aligned(frame, layout->columns, layout->rows, row, column,
						 width, compact, style, right);
}

static void cli_resume_rule(morph_buf_t *frame, int row, int width)
{
	morph_buf_printf(frame, "\033[%d;3H%s", row, CLI_RESUME_RULE);
	for (int i = 0; i < width; i++)
		morph_buf_puts(frame, "─");
}

static int cli_resume_frame(morph_buf_t *frame, struct session *list, int count, int focus,
			    const struct cli_resume_layout *layout)
{
	int page = focus / layout->page_size;
	int start = page * layout->page_size;
	int end = count - start > layout->page_size ? start + layout->page_size : count;
	char label[128];
	morph_buf_t tokens;
	int rc = morph_buf_init(&tokens, 32);

	if (rc != 0)
		return rc;
	morph_buf_reset(frame);
	morph_buf_printf(frame, "\033[?2026h%s\033[H\033[J", CLI_RESUME_NORMAL);
	if (layout->columns < 5) {
		cli_resume_cell(frame, layout, 1, 1, layout->columns, list[focus].name,
				CLI_RESUME_SELECTED, 0);
		morph_buf_puts(frame, "\033[0m\033[?2026l");
		morph_buf_cleanup(&tokens);
		return morph_tui_draw_write(stdout, frame);
	}
	if (layout->rows >= 14) {
		cli_resume_cell(frame, layout, 1, 3, 5, "morph", CLI_RESUME_ACCENT, 0);
		cli_resume_cell(frame, layout, 1, 9, layout->width - 6, "/ sessions",
				CLI_RESUME_MUTED, 0);
		cli_resume_cell(frame, layout, 3, 3, layout->width, "Resume a session",
				CLI_RESUME_NORMAL "\033[1m", 0);
		snprintf(label, sizeof(label), "%d sessions · sorted by recent activity", count);
		cli_resume_cell(frame, layout, 4, 3, layout->width, label, CLI_RESUME_MUTED, 0);
		cli_resume_rule(frame, 6, layout->width);
	} else if (layout->rows >= 3) {
		cli_resume_cell(frame, layout, 1, 3, layout->width, "Sessions", CLI_RESUME_ACCENT,
				0);
	}
	if (layout->rows >= 3 && layout->width >= 28) {
		snprintf(label, sizeof(label), "%d / %d", page + 1,
			 (count - 1) / layout->page_size + 1);
		int width = (int)strlen(label);

		cli_resume_cell(frame, layout, layout->rows >= 14 ? 3 : 1,
				3 + layout->width - width, width, label, CLI_RESUME_MUTED, 1);
	}
	if (layout->rows >= 6) {
		int row = layout->first_row - 1;

		cli_resume_cell(frame, layout, row, 3, layout->name_width, "SESSION",
				CLI_RESUME_MUTED, 0);
		if (layout->id_column)
			cli_resume_cell(frame, layout, row, layout->id_column, 8, "ID",
					CLI_RESUME_MUTED, 0);
		if (layout->tokens_column)
			cli_resume_cell(frame, layout, row, layout->tokens_column, 12, "TOKENS",
					CLI_RESUME_MUTED, 1);
		if (layout->updated_column)
			cli_resume_cell(frame, layout, row, layout->updated_column, 14, "UPDATED",
					CLI_RESUME_MUTED, 0);
	}
	for (int i = start; i < end; i++) {
		int row = layout->first_row + i - start;
		int selected = i == focus;
		const char *text = selected ? CLI_RESUME_SELECTED "\033[1m" : CLI_RESUME_NORMAL;
		const char *metadata = selected ? CLI_RESUME_SELECTED_META : CLI_RESUME_MUTED;

		cli_resume_cell(frame, layout, row, 3, layout->width, "", text, 0);
		if (selected && layout->columns > 4)
			cli_resume_cell(frame, layout, row, 2, 1, "▏", CLI_RESUME_BAR, 0);
		cli_resume_cell(frame, layout, row, 3, layout->name_width, list[i].name, text, 0);
		if (layout->id_column)
			cli_resume_cell(frame, layout, row, layout->id_column, 8,
					list[i].display_id, metadata, 0);
		if (layout->tokens_column) {
			morph_buf_reset(&tokens);
			rc = cli_resume_append_token_number(&tokens, list[i].token_used);
			if (rc != 0)
				break;
			cli_resume_cell(frame, layout, row, layout->tokens_column, 12,
					morph_buf_cstr(&tokens), metadata, 1);
		}
		if (layout->updated_column) {
			morph_buf_reset(&tokens);
			rc = cli_resume_append_session_time(&tokens, list[i].updated_at,
							    list[i].created_at);
			if (rc != 0)
				break;
			cli_resume_cell(frame, layout, row, layout->updated_column, 14,
					morph_buf_cstr(&tokens) + strlen(" · "), metadata, 0);
		}
	}
	if (layout->rows >= 6) {
		snprintf(label, sizeof(label), "Showing %d–%d of %d", start + 1, end, count);
		if (layout->rows >= 14)
			cli_resume_rule(frame, layout->rows - 4, layout->width);
		cli_resume_cell(frame, layout, layout->rows - 3 + (layout->rows < 14), 3,
				layout->width, label, CLI_RESUME_MUTED, 0);
		cli_resume_rule(frame, layout->rows - 1, layout->width);
	}
	if (layout->rows >= 3) {
		const char *hint = layout->width >= 68 ? "↑ ↓  Select    Enter  Resume    ← →  Page"
				   : layout->width >= 36 ? "↑↓ Select  Enter Resume  Esc Cancel"
							 : "↑↓ Enter Esc";

		cli_resume_cell(frame, layout, layout->rows, 3, layout->width, hint,
				CLI_RESUME_MUTED, 0);
		if (layout->width >= 68)
			cli_resume_cell(frame, layout, layout->rows, 3 + layout->width - 10, 10,
					"Esc Cancel", CLI_RESUME_NORMAL, 1);
	}
	morph_buf_puts(frame, "\033[0m\033[?2026l");
	if (rc == 0)
		rc = morph_tui_draw_write(stdout, frame);
	morph_buf_cleanup(&tokens);
	return rc;
}

struct cli_resume_picker {
	struct cli_application *ctx;
	int selection;
	struct session *sessions;
	int count;
	int *selected;
	struct morph_tui_widget_menu_state menu;
	struct cli_resume_layout layout;
	morph_buf_t frame;
};

static int cli_resume_draw(void *data, const struct morph_tui_screen *screen)
{
	struct cli_resume_picker *picker = data;

	picker->layout = cli_resume_dimensions(screen);
	morph_tui_widget_list_state_layout(&picker->menu.list, (size_t)picker->count,
					   (size_t)picker->layout.page_size);
	return cli_resume_frame(&picker->frame, picker->sessions, picker->count,
				(int)picker->menu.list.index, &picker->layout);
}

static int cli_resume_update(void *data, const struct morph_tui_event *event)
{
	struct cli_resume_picker *picker = data;

	if (event->type != MORPH_TUI_EVENT_KEY || event->pasting)
		return MORPH_TUI_SCREEN_CONTINUE;
	int key = event->key;

	if (key == 'q')
		return MORPH_TUI_SCREEN_CLOSE;
	if (key == 'n')
		key = MORPH_TUI_KEY_PAGE_DOWN;
	if (key == 'p')
		key = MORPH_TUI_KEY_PAGE_UP;
	int result = morph_tui_widget_menu_state_update(&picker->menu, key);

	if (result == MORPH_TUI_WIDGET_SUBMIT) {
		*picker->selected = (int)picker->menu.list.index;
		return MORPH_TUI_SCREEN_CLOSE;
	}
	if (result == MORPH_TUI_WIDGET_BACK || result == MORPH_TUI_WIDGET_CANCEL)
		return MORPH_TUI_SCREEN_CLOSE;
	return result == MORPH_TUI_WIDGET_CHANGED ? MORPH_TUI_SCREEN_REDRAW
						  : MORPH_TUI_SCREEN_CONTINUE;
}

/* Own a temporary screen so navigation never fills the transcript. */
static int cli_resume_pick(struct session *list, int count, int *selected)
{
	struct cli_resume_picker picker = {.sessions = list, .count = count, .selected = selected};
	struct morph_tui_screen screen = {0};
	const struct morph_tui_screen_ops ops = {.update = cli_resume_update,
						 .draw = cli_resume_draw};
	int rc = morph_buf_init(&picker.frame, BUFSIZ);

	*selected = -1;
	picker.menu.list.flags = MORPH_TUI_WIDGET_LIST_VIM | MORPH_TUI_WIDGET_LIST_TAB |
				 MORPH_TUI_WIDGET_LIST_HORIZONTAL_PAGES |
				 MORPH_TUI_WIDGET_LIST_PAGED;
	if (rc == 0)
		rc = morph_tui_screen_open(&screen, stdin, stdout,
					   MORPH_TUI_TERMINAL_SESSION_ALTERNATE |
						   MORPH_TUI_TERMINAL_SESSION_PASTE);
	if (rc == 0)
		rc = morph_tui_screen_run(&screen, &ops, &picker);
	int leave_rc = morph_tui_screen_close(&screen);

	if (rc == 0)
		rc = leave_rc;
	morph_buf_cleanup(&picker.frame);
	return rc;
}

static int cli_resume_select(struct cli_application *ctx, int64_t id)
{
	struct session selected;
	int rc = runtime_session_select_existing(ctx->runtime, id, &selected);

	if (rc) {
		CMD_ERROR(ctx, "failed to resume session: %s", morph_strerror(rc));
		return rc;
	}
	ctx->session_auto_named = 1;
	CMD_OK(ctx, "resumed session: %s", selected.name);
	rc = cli_session_history_restore(ctx);
	if (rc)
		CMD_ERROR(ctx, "failed to restore session history: %s", morph_strerror(rc));
	return rc;
}

static void cli_resume_close(void *data, int result)
{
	struct cli_resume_picker *picker = data;
	struct cli_application *ctx = picker->ctx;

	if (!result && picker->selection >= 0) {
		(void)cli_resume_select(ctx, picker->sessions[picker->selection].id);
	}
	runtime_session_list_free(picker->sessions);
	morph_buf_cleanup(&picker->frame);
	free(picker);
}

static int cli_resume_present(struct cli_application *ctx, struct session *list, int count)
{
	struct cli_resume_picker *picker = calloc(1, sizeof(*picker));
	const struct morph_tui_screen_ops ops = {.update = cli_resume_update,
						 .draw = cli_resume_draw};

	if (!picker)
		return -ENOMEM;
	picker->ctx = ctx;
	picker->sessions = list;
	picker->count = count;
	picker->selection = -1;
	picker->selected = &picker->selection;
	picker->menu.list.flags = MORPH_TUI_WIDGET_LIST_VIM | MORPH_TUI_WIDGET_LIST_TAB |
				  MORPH_TUI_WIDGET_LIST_HORIZONTAL_PAGES |
				  MORPH_TUI_WIDGET_LIST_PAGED;
	int rc = morph_buf_init(&picker->frame, BUFSIZ);

	if (!rc)
		rc = cli_application_present_screen(ctx, &ops, picker, cli_resume_close);
	if (rc) {
		morph_buf_cleanup(&picker->frame);
		free(picker);
	}
	return rc;
}

int cli_resume_show(struct cli_application *ctx)
{
	struct session *list = NULL;
	int64_t current_id = 0;
	int count = 0;
	int page = 0;
	int rc;

	if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON) {
		CMD_ERROR(ctx, "use /resume <name|id> in JSON mode");
		return -EINVAL;
	}
	rc = runtime_session_list_query(ctx->runtime, &list, &count, 0, NULL);
	if (rc != 0)
		return rc;
	(void)runtime_session_current_id(ctx->runtime, &current_id);
	if (count == 0)
		CMD_OK(ctx, "no sessions to resume");
	if (count > 0 && cli_setup_controls_available(stdin, stdout)) {
		if (cli_application_tui(ctx)) {
			rc = cli_resume_present(ctx, list, count);
			if (rc)
				runtime_session_list_free(list);
			return rc;
		}
		int index;

		rc = cli_resume_pick(list, count, &index);
		if (rc == 0 && index >= 0)
			rc = cli_resume_select(ctx, list[index].id);
		runtime_session_list_free(list);
		return rc;
	}
	while (count > 0) {
		char answer[BUFSIZ];
		char *end;
		long number;

		rc = cli_resume_page(ctx, list, count, page, current_id, CLI_RESUME_PAGE_SIZE);
		if (rc != 0)
			break;
		printf(ctx, "  Select 1-%d · n next · p previous · q cancel: ",
		       count - page * CLI_RESUME_PAGE_SIZE > CLI_RESUME_PAGE_SIZE
			       ? CLI_RESUME_PAGE_SIZE
			       : count - page * CLI_RESUME_PAGE_SIZE);
		fflush(stdout);
		if (!fgets(answer, sizeof(answer), stdin))
			break;
		if (!strchr(answer, '\n') && !feof(stdin)) {
			int ch;

			while ((ch = getchar()) != '\n' && ch != EOF) {}
			CMD_ERROR(ctx, "invalid selection");
			continue;
		}
		answer[strcspn(answer, "\r\n")] = '\0';
		if (!answer[0] || strcmp(answer, "q") == 0)
			break;
		if (strcmp(answer, "n") == 0) {
			if (page < (count - 1) / CLI_RESUME_PAGE_SIZE)
				page++;
			else
				CMD_ERROR(ctx, "already on the last page");
			continue;
		}
		if (strcmp(answer, "p") == 0) {
			if (page > 0)
				page--;
			else
				CMD_ERROR(ctx, "already on the first page");
			continue;
		}
		errno = 0;
		number = strtol(answer, &end, 10);
		if (errno || end == answer || *end || number < 1 || number > CLI_RESUME_PAGE_SIZE ||
		    number > count - page * CLI_RESUME_PAGE_SIZE) {
			CMD_ERROR(ctx, "invalid selection");
			continue;
		}
		rc = cli_resume_select(ctx,
			list[page * CLI_RESUME_PAGE_SIZE + (int)number - 1].id);
		break;
	}
	runtime_session_list_free(list);
	return rc;
}
