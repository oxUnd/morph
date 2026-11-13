#include "sapi/cli/pages/resume.h"
#include "sapi/cli/presentation/output.h"
#include "runtime/runtime.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/widgets/list.h"
#include "sapi/cli/pages/setup/controls.h"
#include "sapi/cli/application.h"
#include "sapi/cli/services/session_restore.h"
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

struct cli_resume_group {
	const char *workdir;
	int count;
	int current;
};

struct cli_resume_model {
	const struct session *sessions;
	int session_count;
	struct cli_resume_group *groups;
	int group_count;
	int *order; /* item ordinal -> sessions[] index */
	struct morph_tui_widget_list_groups rows;
};

static void cli_resume_model_cleanup(struct cli_resume_model *model)
{
	if (!model)
		return;
	free(model->groups);
	free(model->order);
	morph_tui_widget_list_groups_cleanup(&model->rows);
	memset(model, 0, sizeof(*model));
}

static int cli_resume_model_group(const struct cli_resume_model *model, const char *workdir)
{
	for (int i = 0; i < model->group_count; i++) {
		if (strcmp(model->groups[i].workdir, workdir) == 0)
			return i;
	}
	return -1;
}

static void cli_resume_model_accumulate(struct cli_resume_model *model, int *session_groups,
					const char *current_workdir)
{
	for (int i = 0; i < model->session_count; i++) {
		const char *workdir = model->sessions[i].workdir;
		int group = cli_resume_model_group(model, workdir);

		if (group < 0) {
			group = model->group_count++;
			model->groups[group].workdir = workdir;
			model->groups[group].current =
				workdir[0] && current_workdir && current_workdir[0] &&
				strcmp(workdir, current_workdir) == 0;
		}
		model->groups[group].count++;
		session_groups[i] = group;
	}
}

static void cli_resume_model_order(const struct cli_resume_model *model, int *order, int *count)
{
	int current_group = -1;

	for (int g = 0; g < model->group_count; g++) {
		if (model->groups[g].current)
			current_group = g;
	}
	*count = 0;
	if (current_group >= 0)
		order[(*count)++] = current_group;
	for (int g = 0; g < model->group_count; g++) {
		if (g != current_group)
			order[(*count)++] = g;
	}
}

static int cli_resume_model_fill(struct cli_resume_model *model, const int *session_groups,
				 const int *order, size_t *counts)
{
	int item = 0;

	for (int oi = 0; oi < model->group_count; oi++) {
		int group = order[oi];

		counts[oi] = (size_t)model->groups[group].count;
		for (int i = 0; i < model->session_count; i++) {
			if (session_groups[i] != group)
				continue;
			model->order[item] = i;
			item++;
		}
	}
	return morph_tui_widget_list_groups_init(&model->rows, counts,
						 (size_t)model->group_count);
}

static int cli_resume_model_build(struct cli_resume_model *model, const struct session *sessions,
				  int count, const char *current_workdir)
{
	int *session_groups;
	int *order;
	size_t *counts;
	int ordered = 0;
	int rc;

	if (!model || !sessions || count <= 0)
		return -EINVAL;
	memset(model, 0, sizeof(*model));
	model->sessions = sessions;
	model->session_count = count;
	model->groups = calloc((size_t)count, sizeof(*model->groups));
	model->order = calloc((size_t)count, sizeof(*model->order));
	session_groups = calloc((size_t)count, sizeof(*session_groups));
	order = calloc((size_t)count, sizeof(*order));
	counts = calloc((size_t)count, sizeof(*counts));
	if (!model->groups || !model->order || !session_groups || !order || !counts) {
		free(session_groups);
		free(order);
		free(counts);
		cli_resume_model_cleanup(model);
		return -ENOMEM;
	}
	cli_resume_model_accumulate(model, session_groups, current_workdir);
	cli_resume_model_order(model, order, &ordered);
	rc = cli_resume_model_fill(model, session_groups, order, counts);
	free(session_groups);
	free(order);
	free(counts);
	if (rc != 0) {
		cli_resume_model_cleanup(model);
		return rc;
	}
	return 0;
}

static int cli_resume_model_first_row(const struct cli_resume_model *model)
{
	return model->rows.item_count ? (int)model->rows.item_row[0] : 0;
}

static const struct session *cli_resume_model_session(const struct cli_resume_model *model,
						      int ordinal)
{
	if (!model || ordinal < 0 || ordinal >= model->session_count)
		return NULL;
	return &model->sessions[model->order[ordinal]];
}

static int cli_resume_model_row_item(const struct cli_resume_model *model, int row)
{
	if (!model || row < 0 || (size_t)row >= model->rows.row_count ||
	    model->rows.row_item[row] == SIZE_MAX)
		return -1;
	return (int)model->rows.row_item[row];
}

static const struct session *cli_resume_model_row_session(const struct cli_resume_model *model,
							  int row)
{
	int item = cli_resume_model_row_item(model, row);

	return item >= 0 ? cli_resume_model_session(model, item) : NULL;
}

static const struct cli_resume_group *cli_resume_model_row_group(
	const struct cli_resume_model *model, int row)
{
	if (!model || row < 0 || (size_t)row >= model->rows.row_count)
		return NULL;
	return &model->groups[model->rows.row_group[row]];
}

static int cli_resume_append_group(morph_buf_t *buf, const struct cli_resume_group *group)
{
	const char *workdir = group->workdir;
	const char *home = getenv("HOME");
	size_t home_length = home ? strlen(home) : 0;
	int rc;

	if (!workdir || !workdir[0]) {
		rc = morph_buf_puts(buf, "unknown workdir");
	} else if (home_length > 0 && strncmp(workdir, home, home_length) == 0 &&
		   (workdir[home_length] == '\0' || workdir[home_length] == '/')) {
		rc = morph_buf_putc(buf, '~');
		if (rc == 0)
			rc = morph_buf_puts(buf, workdir + home_length);
	} else {
		rc = morph_buf_puts(buf, workdir);
	}
	if (rc == 0 && group->current)
		rc = morph_buf_puts(buf, " · current");
	return rc;
}

static int cli_resume_page(struct cli_application *ctx, const struct cli_resume_model *model,
			   int page, int64_t current_id, int page_size)
{
	int count = model->session_count;
	int start = page * page_size;
	int end = count - start > page_size ? start + page_size : count;

	CMD_HEADER(ctx, "resume · page %d/%d · %d sessions", page + 1, (count - 1) / page_size + 1,
		   count);
	for (int ordinal = start; ordinal < end; ordinal++) {
		const struct session *session = cli_resume_model_session(model, ordinal);
		int row = (int)model->rows.item_row[ordinal];
		int group = (int)model->rows.row_group[row];
		char number[16];
		morph_buf_t metadata;
		int rc;

		if (ordinal == start ||
		    group != (int)model->rows.row_group[model->rows.item_row[ordinal - 1]]) {
			morph_buf_t label;

			rc = morph_buf_init(&label, 64);
			if (rc != 0)
				return rc;
			rc = cli_resume_append_group(&label, &model->groups[group]);
			if (rc == 0)
				cli_list_group(ctx, morph_buf_cstr_borrow(&label),
					       model->groups[group].count, 0);
			morph_buf_cleanup(&label);
			if (rc != 0)
				return rc;
		}
		rc = morph_buf_init(&metadata, 64);
		if (rc != 0)
			return rc;
		rc = morph_buf_printf(&metadata, "%s · ", session->display_id);
		if (rc == 0)
			rc = cli_resume_append_token_count(&metadata, session->token_used);
		if (rc == 0)
			rc = cli_resume_append_session_time(&metadata, session->updated_at,
							    session->created_at);
		if (rc == 0) {
			snprintf(number, sizeof(number), "%d", ordinal - start + 1);
			cli_list_row(ctx, number, session->name,
				     morph_buf_cstr_borrow(&metadata), session->id == current_id,
				     ordinal == end - 1, cli_list_columns());
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
	int grouped;
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
	layout.grouped = layout.rows >= 14;
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

static const struct session *cli_resume_focus_session(const struct cli_resume_model *model,
						      const struct cli_resume_layout *layout,
						      int focus)
{
	if (layout->grouped) {
		if (focus < 0 || (size_t)focus >= model->rows.row_count)
			return NULL;
		return cli_resume_model_row_session(model, focus);
	}
	return cli_resume_model_session(model, focus);
}

static int cli_resume_frame(morph_buf_t *frame, const struct cli_resume_model *model, int focus,
			    const struct cli_resume_layout *layout)
{
	int count = layout->grouped ? (int)model->rows.row_count : model->session_count;
	int page = count > 0 ? focus / layout->page_size : 0;
	int start = page * layout->page_size;
	int end = count - start > layout->page_size ? start + layout->page_size : count;
	int pages = count > 0 ? (count - 1) / layout->page_size + 1 : 0;
	int shown_first = 0;
	int shown_last = 0;
	char label[128];
	morph_buf_t tokens;
	int rc = morph_buf_init(&tokens, 32);

	if (rc != 0)
		return rc;
	morph_buf_reset(frame);
	morph_buf_printf(frame, "\033[?2026h%s\033[H\033[J", CLI_RESUME_NORMAL);
	if (layout->columns < 5) {
		const struct session *session = cli_resume_focus_session(model, layout, focus);

		cli_resume_cell(frame, layout, 1, 1, layout->columns,
				session ? session->name : "", CLI_RESUME_SELECTED, 0);
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
		snprintf(label, sizeof(label), "%d sessions · grouped by working directory",
			 model->session_count);
		cli_resume_cell(frame, layout, 4, 3, layout->width, label, CLI_RESUME_MUTED, 0);
		cli_resume_rule(frame, 6, layout->width);
	} else if (layout->rows >= 3) {
		cli_resume_cell(frame, layout, 1, 3, layout->width, "Sessions", CLI_RESUME_ACCENT,
				0);
	}
	if (layout->rows >= 3 && layout->width >= 28) {
		snprintf(label, sizeof(label), "%d / %d", page + 1, pages);
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
		int selected;
		int last_in_group = 0;
		const struct session *session;
		const char *text;
		const char *metadata;

		if (layout->grouped) {
			const struct cli_resume_group *group =
				cli_resume_model_row_group(model, i);
			int item = cli_resume_model_row_item(model, i);

			if (!group || item < 0) {
				morph_buf_reset(&tokens);
				if (!group) {
					rc = -EINVAL;
					break;
				}
				rc = cli_resume_append_group(&tokens, group);
				if (rc != 0)
					break;
				cli_resume_cell(frame, layout, row, 3, 1, "▾",
						group->current ? CLI_RESUME_ACCENT
							       : CLI_RESUME_MUTED,
						0);
				cli_resume_cell(frame, layout, row, 5, layout->width - 2,
						morph_buf_cstr_borrow(&tokens),
						group->current ? CLI_RESUME_ACCENT
							       : CLI_RESUME_MUTED,
						0);
				continue;
			}
			session = cli_resume_model_session(model, item);
			selected = i == focus;
			last_in_group = i + 1 >= count ||
					model->rows.row_group[i + 1] !=
						model->rows.row_group[i];
			if (!shown_last)
				shown_first = item + 1;
			shown_last = item + 1;
		} else {
			session = cli_resume_model_session(model, i);
			selected = i == focus;
			if (!shown_last)
				shown_first = i + 1;
			shown_last = i + 1;
		}
		text = selected ? CLI_RESUME_SELECTED "\033[1m" : CLI_RESUME_NORMAL;
		metadata = selected ? CLI_RESUME_SELECTED_META : CLI_RESUME_MUTED;
		cli_resume_cell(frame, layout, row, 3, layout->width, "", text, 0);
		if (selected && layout->columns > 4)
			cli_resume_cell(frame, layout, row, 2, 1, "▏", CLI_RESUME_BAR, 0);
		if (layout->grouped) {
			int name_width = layout->name_width - 4;

			if (name_width < 1)
				name_width = 1;
			cli_resume_cell(frame, layout, row, 5, 1,
					last_in_group ? "└" : "├", metadata, 0);
			cli_resume_cell(frame, layout, row, 7, name_width, session->name,
					text, 0);
		} else {
			cli_resume_cell(frame, layout, row, 3, layout->name_width, session->name,
					text, 0);
		}
		if (layout->id_column)
			cli_resume_cell(frame, layout, row, layout->id_column, 8,
					session->display_id, metadata, 0);
		if (layout->tokens_column) {
			morph_buf_reset(&tokens);
			rc = cli_resume_append_token_number(&tokens, session->token_used);
			if (rc != 0)
				break;
			cli_resume_cell(frame, layout, row, layout->tokens_column, 12,
					morph_buf_cstr_borrow(&tokens), metadata, 1);
		}
		if (layout->updated_column) {
			morph_buf_reset(&tokens);
			rc = cli_resume_append_session_time(&tokens, session->updated_at,
							    session->created_at);
			if (rc != 0)
				break;
			cli_resume_cell(frame, layout, row, layout->updated_column, 14,
					morph_buf_cstr_borrow(&tokens) + strlen(" · "), metadata,
					0);
		}
	}
	if (layout->rows >= 6) {
		if (shown_last > 0)
			snprintf(label, sizeof(label), "Showing %d–%d of %d", shown_first,
				 shown_last, model->session_count);
		else
			snprintf(label, sizeof(label), "No sessions");
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
	struct cli_resume_model *model;
	int *selected;
	int initialized;
	int last_grouped;
	struct morph_tui_widget_menu_state menu;
	struct cli_resume_layout layout;
	morph_buf_t frame;
};

/* Keep the same session selected when the terminal crosses the grouping threshold. */
static void cli_resume_preserve_selection(struct cli_resume_picker *picker)
{
	const struct morph_tui_widget_list_groups *rows = &picker->model->rows;
	size_t item;

	if (picker->last_grouped) {
		size_t row = picker->menu.list.index;

		item = row < rows->row_count ? rows->row_item[row] : SIZE_MAX;
	} else {
		item = picker->menu.list.index;
	}
	if (item == SIZE_MAX || item >= rows->item_count)
		item = 0;
	picker->menu.list.index = picker->layout.grouped ? rows->item_row[item] : item;
	picker->last_grouped = picker->layout.grouped;
}

static int cli_resume_draw(void *data, const struct morph_tui_screen *screen)
{
	struct cli_resume_picker *picker = data;
	int total;

	picker->layout = cli_resume_dimensions(screen);
	total = picker->layout.grouped ? (int)picker->model->rows.row_count
				       : picker->model->session_count;
	if (!picker->initialized) {
		picker->menu.list.index = picker->layout.grouped
						 ? (size_t)cli_resume_model_first_row(picker->model)
						 : 0;
		picker->initialized = 1;
		picker->last_grouped = picker->layout.grouped;
	} else if (picker->last_grouped != picker->layout.grouped) {
		cli_resume_preserve_selection(picker);
	}
	if (picker->layout.grouped)
		morph_tui_widget_list_state_layout_groups(&picker->menu.list,
							  &picker->model->rows,
							  (size_t)picker->layout.page_size);
	else
		morph_tui_widget_list_state_layout(&picker->menu.list, (size_t)total,
						   (size_t)picker->layout.page_size);
	return cli_resume_frame(&picker->frame, picker->model,
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
	int result;

	if (picker->layout.grouped)
		result = morph_tui_widget_menu_state_update_groups(
			&picker->menu, &picker->model->rows,
			(size_t)picker->layout.page_size, key);
	else {
		morph_tui_widget_list_state_layout(&picker->menu.list,
						   (size_t)picker->model->session_count,
						   (size_t)picker->layout.page_size);
		result = morph_tui_widget_menu_state_update(&picker->menu, key);
	}

	if (result == MORPH_TUI_WIDGET_SUBMIT) {
		size_t item = picker->layout.grouped
				      ? picker->model->rows.row_item[picker->menu.list.index]
				      : picker->menu.list.index;

		if (item < (size_t)picker->model->session_count)
			*picker->selected = picker->model->order[item];
		return MORPH_TUI_SCREEN_CLOSE;
	}
	if (result == MORPH_TUI_WIDGET_BACK || result == MORPH_TUI_WIDGET_CANCEL)
		return MORPH_TUI_SCREEN_CLOSE;
	return result == MORPH_TUI_WIDGET_CHANGED ? MORPH_TUI_SCREEN_REDRAW
						  : MORPH_TUI_SCREEN_CONTINUE;
}

/* Own a temporary screen so navigation never fills the transcript. */
static int cli_resume_pick(struct cli_resume_model *model, int *selected)
{
	struct cli_resume_picker picker = {.model = model, .selected = selected};
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
		(void)cli_resume_select(ctx, picker->model->sessions[picker->selection].id);
	}
	runtime_session_list_free((struct session *)picker->model->sessions);
	cli_resume_model_cleanup(picker->model);
	free(picker->model);
	morph_buf_cleanup(&picker->frame);
	free(picker);
}

static int cli_resume_present(struct cli_application *ctx, struct cli_resume_model *model)
{
	struct cli_resume_picker *picker = calloc(1, sizeof(*picker));
	const struct morph_tui_screen_ops ops = {.update = cli_resume_update,
						 .draw = cli_resume_draw};

	if (!picker)
		return -ENOMEM;
	picker->ctx = ctx;
	picker->model = calloc(1, sizeof(*picker->model));
	if (!picker->model) {
		free(picker);
		return -ENOMEM;
	}
	*picker->model = *model;
	memset(model, 0, sizeof(*model));
	picker->selection = -1;
	picker->selected = &picker->selection;
	picker->menu.list.flags = MORPH_TUI_WIDGET_LIST_VIM | MORPH_TUI_WIDGET_LIST_TAB |
				  MORPH_TUI_WIDGET_LIST_HORIZONTAL_PAGES |
				  MORPH_TUI_WIDGET_LIST_PAGED;
	int rc = morph_buf_init(&picker->frame, BUFSIZ);

	if (!rc)
		rc = cli_application_present_screen(ctx, &ops, picker, cli_resume_close);
	if (rc) {
		runtime_session_list_free((struct session *)picker->model->sessions);
		cli_resume_model_cleanup(picker->model);
		free(picker->model);
		morph_buf_cleanup(&picker->frame);
		free(picker);
	}
	return rc;
}

int cli_resume_show(struct cli_application *ctx)
{
	struct session *list = NULL;
	struct cli_resume_model model;
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
	if (count == 0) {
		CMD_OK(ctx, "no sessions to resume");
		runtime_session_list_free(list);
		return 0;
	}
	rc = cli_resume_model_build(&model, list, count, runtime_workdir_borrow(ctx->runtime));
	if (rc != 0) {
		runtime_session_list_free(list);
		return rc;
	}
	(void)runtime_session_current_id(ctx->runtime, &current_id);
	if (cli_setup_controls_available(stdin, stdout)) {
		if (cli_application_tui_borrow(ctx)) {
			rc = cli_resume_present(ctx, &model);
			if (model.sessions)
				runtime_session_list_free(list);
			cli_resume_model_cleanup(&model);
			return rc;
		}
		int index;

		rc = cli_resume_pick(&model, &index);
		if (rc == 0 && index >= 0)
			rc = cli_resume_select(ctx, list[index].id);
		cli_resume_model_cleanup(&model);
		runtime_session_list_free(list);
		return rc;
	}
	while (count > 0) {
		char answer[BUFSIZ];
		char *end;
		long number;

		rc = cli_resume_page(ctx, &model, page, current_id, CLI_RESUME_PAGE_SIZE);
		if (rc != 0)
			break;
		cli_output_printf(ctx, "  Select 1-%d · n next · p previous · q cancel: ",
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
		const struct session *session = cli_resume_model_session(
			&model, page * CLI_RESUME_PAGE_SIZE + (int)number - 1);

		if (!session) {
			CMD_ERROR(ctx, "invalid selection");
			continue;
		}
		rc = cli_resume_select(ctx, session->id);
		break;
	}
	runtime_session_list_free(list);
	return rc;
}
