#include "sapi/cli/page/resume.h"
#include <nb/err.h>
#include <nb/cleanup.h>
#include "sapi/cli/presentation/output.h"
#include "runtime/runtime.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/component/list.h"
#include <nothing/tui/terminal.h>
#include "sapi/cli/application.h"
#include "sapi/cli/services/session_restore.h"
#include <nothing/tui/models/list_navigation.h>
#include <nothing/tui/controls/list_view.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/terminal/text.h>
#include <nothing/tui/screen.h>

static int cli_resume_append_token_number(nb_buf *buf, int64_t tokens)
{
	char raw[32];
	const char *digits;
	size_t length;
	int rc;

	snprintf(raw, sizeof(raw), "%lld", (long long)tokens);
	digits = raw;
	if (*digits == '-') {
		rc = (nb_buf_append_char(buf, *digits++) ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
	}
	length = strlen(digits);
	for (size_t i = 0; i < length; i++) {
		if (i > 0 && (length - i) % 3 == 0) {
			rc = (nb_buf_append_char(buf, ',') ? 0 : NB_ERR_NOMEM);
			if (rc != 0)
				return rc;
		}
		rc = (nb_buf_append_char(buf, digits[i]) ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
	}
	return 0;
}

static int cli_resume_append_token_count(nb_buf *buf, int64_t tokens)
{
	int rc = cli_resume_append_token_number(buf, tokens);

	return rc == 0 ? (nb_buf_append_cstr(buf, " tokens") ? 0 : NB_ERR_NOMEM) : rc;
}

static int cli_resume_append_session_time(nb_buf *buf, int64_t updated_at, int64_t created_at)
{
	char formatted[16];
	struct tm local;
	time_t timestamp;

	timestamp = (time_t)(updated_at > 0 ? updated_at : created_at);
	if (timestamp <= 0 || !localtime_r(&timestamp, &local) ||
	    strftime(formatted, sizeof(formatted), "%m-%d %H:%M", &local) == 0)
		return (nb_buf_append_cstr(buf, " · -- --:--") ? 0 : NB_ERR_NOMEM);
	return (nb_buf_printf(buf, " · %s", formatted) ? 0 : NB_ERR_NOMEM);
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
	int *group_order;
	int *order; /* item ordinal -> sessions[] index */
	struct nt_list_groups rows;
};

static void cli_resume_model_cleanup(struct cli_resume_model *model)
{
	if (!model)
		return;
	free(model->groups);
	free(model->group_order);
	free(model->order);
	nt_list_groups_deinit(&model->rows);
	memset(model, 0, sizeof(*model));
}

static void cli_resume_sessions_cleanup(struct session **sessions)
{
	if (!sessions)
		return;
	runtime_session_list_free(*sessions);
	*sessions = NULL;
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
			model->groups[group].current = workdir[0] && current_workdir &&
						       current_workdir[0] &&
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
	return nt_list_groups_init(&model->rows, counts, (size_t)model->group_count);
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
		return NB_ERR_INVAL;
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
		return NB_ERR_NOMEM;
	}
	cli_resume_model_accumulate(model, session_groups, current_workdir);
	cli_resume_model_order(model, order, &ordered);
	rc = cli_resume_model_fill(model, session_groups, order, counts);
	free(session_groups);
	free(counts);
	if (rc != 0) {
		free(order);
		cli_resume_model_cleanup(model);
		return rc;
	}
	model->group_order = order;
	return 0;
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

static int cli_resume_append_group(nb_buf *buf, const struct cli_resume_group *group)
{
	const char *workdir = group->workdir;
	const char *home = getenv("HOME");
	size_t home_length = home ? strlen(home) : 0;
	int rc;

	if (!workdir || !workdir[0]) {
		rc = (nb_buf_append_cstr(buf, "unknown workdir") ? 0 : NB_ERR_NOMEM);
	} else if (home_length > 0 && strncmp(workdir, home, home_length) == 0 &&
		   (workdir[home_length] == '\0' || workdir[home_length] == '/')) {
		rc = (nb_buf_append_char(buf, '~') ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_cstr(buf, workdir + home_length) ? 0 : NB_ERR_NOMEM);
	} else {
		rc = (nb_buf_append_cstr(buf, workdir) ? 0 : NB_ERR_NOMEM);
	}
	if (rc == 0 && group->current)
		rc = (nb_buf_append_cstr(buf, " · current") ? 0 : NB_ERR_NOMEM);
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
		int group = model->group_order[model->rows.row_group[row]];
		char number[16];
		nb_buf metadata NB_CLEANUP(nb_buf_cleanup) = {0};
		int rc;

		if (ordinal == start ||
		    group != model->group_order
				     [model->rows.row_group[model->rows.item_row[ordinal - 1]]]) {
			nb_buf label NB_CLEANUP(nb_buf_cleanup) = {0};

			rc = (nb_buf_init(&label), nb_buf_reserve(&label, 64) ? 0 : NB_ERR_NOMEM);
			if (rc != 0)
				return rc;
			rc = cli_resume_append_group(&label, &model->groups[group]);
			if (rc == 0)
				cli_list_group(ctx, nb_buf_cstr(&label), model->groups[group].count,
					       0);
			nb_buf_cleanup(&label);
			if (rc != 0)
				return rc;
		}
		rc = (nb_buf_init(&metadata), nb_buf_reserve(&metadata, 64) ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
		rc = (nb_buf_printf(&metadata, "%s · ", session->display_id) ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = cli_resume_append_token_count(&metadata, session->token_used);
		if (rc == 0)
			rc = cli_resume_append_session_time(&metadata, session->updated_at,
							    session->created_at);
		if (rc == 0) {
			snprintf(number, sizeof(number), "%d", ordinal - start + 1);
			cli_list_row(ctx, number, session->name, nb_buf_cstr(&metadata),
				     session->id == current_id, ordinal == end - 1,
				     cli_list_columns());
		}
		nb_buf_cleanup(&metadata);
		if (rc != 0)
			return rc;
	}
	return 0;
}

enum cli_resume_label {
	CLI_RESUME_BRAND,
	CLI_RESUME_PATH,
	CLI_RESUME_TITLE,
	CLI_RESUME_SUMMARY,
	CLI_RESUME_TOP_RULE,
	CLI_RESUME_PAGE,
	CLI_RESUME_NAME,
	CLI_RESUME_ID,
	CLI_RESUME_TOKENS,
	CLI_RESUME_UPDATED,
	CLI_RESUME_BOTTOM_RULE,
	CLI_RESUME_RANGE,
	CLI_RESUME_FOOT_RULE,
	CLI_RESUME_HINT,
	CLI_RESUME_ESCAPE,
	CLI_RESUME_LABEL_COUNT
};

struct cli_resume_picker {
	struct cli_application *ctx;
	struct cli_resume_model model;
	struct session *sessions;
	struct nt_view_controller *controller;
	struct nt_view *list;
	struct nt_view *labels[CLI_RESUME_LABEL_COUNT];
	int rows, columns, grouped, page_size, first_row;
	int name_width, id_x, tokens_x, updated_x;
	int selection;
};

static const struct nt_text_style cli_resume_muted = {.attributes = NT_STYLE_DIM};
static const struct nt_text_style cli_resume_accent = {.foreground = {NT_COLOR_RGB, 0x47d3e6}};
static const struct nt_text_style cli_resume_selected = {.attributes = NT_STYLE_BOLD,
							 .foreground = {NT_COLOR_RGB, 0x47d3e6}};

static int cli_resume_label(struct cli_resume_picker *page, enum cli_resume_label id, int x, int y,
			    int width, const char *text)
{
	struct nt_view *label = page->labels[id];
	if (width > page->columns - x)
		width = page->columns - x;
	int hidden = y < 0 || width <= 0 || page->columns < 5;
	int rc = nt_view_set_hidden(label, hidden);
	if (rc || hidden)
		return rc;
	char clipped[BUFSIZ];
	if (nb_utf8_width_cstr(text) > (size_t)width) {
		nt_terminal_copy_ellipsized_display_width(clipped, sizeof(clipped), text,
							  (size_t)width, 0);
		text = clipped;
	}
	rc = nt_view_set_frame(label, (struct nt_view_rect){x, y, width, 1});
	if (!rc && strcmp(nt_label_text(label), text))
		rc = nt_label_set_text(label, text);
	return rc;
}

static int cli_resume_pagination(struct cli_resume_picker *page)
{
	struct cli_resume_model *model = &page->model;
	size_t index = nt_list_view_selected(page->list);
	int count = page->grouped ? (int)model->rows.row_count : model->session_count;
	int position = page->grouped ? (int)model->rows.item_row[index] : (int)index;
	int number = position / page->page_size;
	int start = number * page->page_size;
	int end = count - start < page->page_size ? count : start + page->page_size;
	int first = 0, last = 0;
	for (int i = start; i < end; i++) {
		int item = page->grouped ? cli_resume_model_row_item(model, i) : i;
		if (item >= 0) {
			if (!first)
				first = item + 1;
			last = item + 1;
		}
	}
	char text[96];
	snprintf(text, sizeof(text), "%d / %d", number + 1, (count - 1) / page->page_size + 1);
	int width = (int)strlen(text);
	int rc = cli_resume_label(
		page, CLI_RESUME_PAGE, page->columns - 2 - width,
		page->columns >= 32 && page->rows >= 3 ? (page->grouped ? 2 : 0) : -1, width, text);
	if (rc)
		return rc;
	snprintf(text, sizeof(text), "Showing %d–%d of %d", first, last, model->session_count);
	return cli_resume_label(page, CLI_RESUME_RANGE, 2,
				page->rows >= 6 ? page->rows - (page->grouped ? 4 : 3) : -1,
				page->columns - 4, text);
}

static int cli_resume_layout(struct cli_resume_picker *page, int cols, int rows)
{
	int width = cols - 4;
	int list_rows = cols < 5 ? 1 : rows;
	page->rows = rows;
	page->columns = cols;
	page->grouped = list_rows >= 14;
	page->first_row = page->grouped ? 7 : list_rows >= 6 ? 2 : list_rows >= 3 ? 1 : 0;
	page->page_size = page->grouped	   ? list_rows - 12
			  : list_rows >= 6 ? list_rows - 5
			  : list_rows >= 3 ? list_rows - 2
					   : 1;
	page->updated_x = width >= 48 ? cols - 16 : 0;
	page->tokens_x = width >= 68 ? page->updated_x - 15 : 0;
	page->id_x = width >= 88 ? page->tokens_x - 11 : 0;
	page->name_width = width - (page->updated_x ? 17 : 0) - (page->tokens_x ? 15 : 0) -
			   (page->id_x ? 11 : 0);
	int rc = nt_list_view_set_groups(page->list, page->grouped ? &page->model.rows : NULL);
	if (!rc)
		rc = nt_view_set_style(page->labels[CLI_RESUME_TITLE],
				       page->grouped
					       ? (struct nt_text_style){.attributes = NT_STYLE_BOLD}
					       : cli_resume_accent);
	if (!rc)
		rc = nt_view_set_frame(page->list, (struct nt_view_rect){0, page->first_row, cols,
									 page->page_size});
	if (rc)
		return rc;
	nb_buf rule NB_CLEANUP(nb_buf_cleanup) = {0};
	for (int i = 0; i < width; i++)
		nb_buf_append_cstr(&rule, "─");
	if (!nb_buf_ok(&rule))
		return NB_ERR_NOMEM;
	int title_row = page->grouped ? 2 : rows >= 3 ? 0 : -1;
	int heading = rows >= 6 ? page->first_row - 1 : -1;
	int footer = rows >= 3 ? rows - 1 : -1;
	char summary[96];
	snprintf(summary, sizeof(summary), "%d sessions · grouped by working directory",
		 page->model.session_count);
	const struct {
		int x, y, width;
		const char *text;
	} labels[CLI_RESUME_LABEL_COUNT] = {
		[CLI_RESUME_BRAND] = {2, page->grouped ? 0 : -1, 5, "morph"},
		[CLI_RESUME_PATH] = {8, page->grouped ? 0 : -1, width - 6, "/ sessions"},
		[CLI_RESUME_TITLE] = {2, title_row, width,
				      page->grouped ? "Resume a session" : "Sessions"},
		[CLI_RESUME_SUMMARY] = {2, page->grouped ? 3 : -1, width, summary},
		[CLI_RESUME_TOP_RULE] = {2, page->grouped ? 5 : -1, width, nb_buf_cstr(&rule)},
		[CLI_RESUME_NAME] = {2, heading, page->name_width, "SESSION"},
		[CLI_RESUME_ID] = {page->id_x, page->id_x ? heading : -1, 8, "ID"},
		[CLI_RESUME_TOKENS] = {page->tokens_x, page->tokens_x ? heading : -1, 12,
				       "      TOKENS"},
		[CLI_RESUME_UPDATED] = {page->updated_x, page->updated_x ? heading : -1, 14,
					"UPDATED"},
		[CLI_RESUME_BOTTOM_RULE] = {2, page->grouped ? rows - 5 : -1, width,
					    nb_buf_cstr(&rule)},
		[CLI_RESUME_FOOT_RULE] = {2, rows >= 6 ? rows - 2 : -1, width, nb_buf_cstr(&rule)},
		[CLI_RESUME_HINT] = {2, footer, width,
				     width >= 68   ? "↑ ↓ Select Enter Resume ← → Page"
				     : width >= 36 ? "↑↓ Select Enter Resume Esc Cancel"
						   : "↑↓ Enter Esc"},
		[CLI_RESUME_ESCAPE] = {cols - 12, width >= 68 ? footer : -1, 10, "Esc Cancel"},
	};
	for (int i = 0; i < CLI_RESUME_LABEL_COUNT && !rc; i++)
		if (i != CLI_RESUME_RANGE && i != CLI_RESUME_PAGE)
			rc = cli_resume_label(page, (enum cli_resume_label)i, labels[i].x,
					      labels[i].y, labels[i].width, labels[i].text);
	return rc ? rc : cli_resume_pagination(page);
}

static int cli_resume_cell(struct nt_canvas *canvas, int x, int width, const char *text,
			   const struct nt_text_style *style, int right)
{
	if (width > canvas->width - x)
		width = canvas->width - x;
	if (width <= 0)
		return 0;
	char compact[BUFSIZ], clipped[BUFSIZ];
	cli_list_compact_text(compact, sizeof(compact), text);
	int used = (int)nt_terminal_copy_ellipsized_display_width(clipped, sizeof(clipped), compact,
								  (size_t)width, 0);
	struct nt_canvas field;
	nt_canvas_subcanvas(canvas, x, 0, width, 1, &field);
	int rc = nt_canvas_fill_styled(&field, style);
	return rc ? rc : nt_canvas_text_styled(&field, right ? width - used : 0, 0, clipped, style);
}

static int cli_resume_row(void *data, struct nt_canvas *canvas, size_t item, size_t group,
			  int selected)
{
	struct cli_resume_picker *page = data;
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	if (item == SIZE_MAX) {
		const struct cli_resume_group *entry =
			&page->model.groups[page->model.group_order[group]];
		const struct nt_text_style *style =
			entry->current ? &cli_resume_accent : &cli_resume_muted;
		int rc = cli_resume_append_group(&text, entry);
		if (!rc)
			rc = cli_resume_cell(canvas, 2, 1, "▾", style, 0);
		return rc ? rc
			  : cli_resume_cell(canvas, 4, canvas->width - 6, nb_buf_cstr(&text), style,
					    0);
	}
	const struct session *session = cli_resume_model_session(&page->model, (int)item);
	const struct nt_text_style *meta = selected ? &cli_resume_accent : &cli_resume_muted;
	const struct nt_text_style normal = {0};
	const struct nt_text_style *name = selected ? &cli_resume_selected : &normal;
	if (canvas->width < 5)
		return cli_resume_cell(canvas, 0, canvas->width, session->name, name, 0);
	int rc = selected ? cli_resume_cell(canvas, 1, 1, "▏", &cli_resume_accent, 0) : 0;
	if (!rc && selected) {
		struct nt_canvas strip;
		nt_canvas_subcanvas(canvas, 2, 0, canvas->width - 4, 1, &strip);
		rc = nt_canvas_fill_styled(&strip, name);
	}
	if (!rc && page->grouped) {
		size_t row = page->model.rows.item_row[item];
		int last = row + 1 == page->model.rows.row_count ||
			   page->model.rows.row_group[row + 1] != group;
		rc = cli_resume_cell(canvas, 4, 1, last ? "└" : "├", meta, 0);
	}
	int inset = page->grouped ? 4 : 2;
	int name_width = page->name_width > inset ? page->name_width - inset : 1;
	if (!rc)
		rc = cli_resume_cell(canvas, 2 + inset, name_width, session->name, name, 0);
	if (!rc && page->id_x)
		rc = cli_resume_cell(canvas, page->id_x, 8, session->display_id, meta, 0);
	if (!rc && page->tokens_x) {
		rc = cli_resume_append_token_number(&text, session->token_used);
		if (!rc)
			rc = cli_resume_cell(canvas, page->tokens_x, 12, nb_buf_cstr(&text), meta,
					     1);
	}
	if (!rc && page->updated_x) {
		nb_buf_clear(&text);
		rc = cli_resume_append_session_time(&text, session->updated_at,
						    session->created_at);
		if (!rc)
			rc = cli_resume_cell(canvas, page->updated_x, 14,
					     nb_buf_cstr(&text) + strlen(" · "), meta, 0);
	}
	return rc;
}

static int cli_resume_action(void *data, const struct nt_view_event *event)
{
	struct cli_resume_picker *page = data;
	if (event->type == NT_VIEW_EVENT_CHANGED)
		return cli_resume_pagination(page);
	page->selection = page->model.order[event->index];
	return nt_view_controller_dismiss(page->controller, 0);
}

static void cli_resume_build(struct nt_view_controller *controller, struct nt_builder *builder)
{
	struct cli_resume_picker *page = controller->data;
	page->controller = controller;
	struct nt_view *root = nt_view_add(builder, NULL, &nt_view_base_class, NULL);
	for (int i = 0; i < CLI_RESUME_LABEL_COUNT; i++) {
		struct nt_view_options options = {.style = cli_resume_muted};
		if (i == CLI_RESUME_BRAND)
			options.style = cli_resume_accent;
		if (i == CLI_RESUME_ESCAPE)
			options.style = (struct nt_text_style){0};
		page->labels[i] = nt_label_add(builder, root, "", &options);
		nt_builder_fail(builder, nt_label_set_opaque(page->labels[i], 1));
		nt_builder_fail(builder,
				nt_label_set_wrap_mode(page->labels[i], NT_TEXT_WRAP_NONE));
	}
	page->list = nt_list_view_add(builder, root, NULL, 0, 0, NULL);
	if (nt_builder_error(builder))
		return;
	int rc = nt_list_view_set_source(page->list, (size_t)page->model.session_count, NULL, NULL);
	if (!rc)
		rc = nt_list_view_set_row_renderer(page->list, cli_resume_row, page);
	if (!rc)
		rc = nt_list_view_set_activate_on_click(page->list, 1);
	if (!rc)
		rc = nt_list_view_set_navigation(page->list, NT_LIST_VIEW_VIM | NT_LIST_VIEW_TAB |
								     NT_LIST_VIEW_HORIZONTAL_PAGES |
								     NT_LIST_VIEW_PAGED);
	struct nt_terminal_environment env;
	if (!rc)
		rc = nt_application_environment(&page->ctx->base, &env);
	if (!rc)
		rc = cli_resume_layout(page, env.columns, env.rows);
	if (!rc && (!nt_view_on(page->list, NT_VIEW_EVENT_ACTIVATE, cli_resume_action, page) ||
		    !nt_view_on(page->list, NT_VIEW_EVENT_CHANGED, cli_resume_action, page)))
		rc = NB_ERR_NOMEM;
	nt_builder_fail(builder, rc);
}

static int cli_resume_event(struct nt_view_controller *controller, const struct nt_event *event)
{
	struct cli_resume_picker *page = controller->data;
	if (event->type == NT_EVENT_RESIZE)
		return cli_resume_layout(page, event->columns, event->rows);
	if (event->type != NT_EVENT_KEY || event->pasting || event->action == NT_KEY_ACTION_RELEASE)
		return 0;
	if (event->key == 'q' || event->key == 27 || event->key == NT_KEY_ESCAPE ||
	    event->key == 3 || event->key == 4)
		return nt_view_controller_dismiss(controller, 0);
	if (event->key == 'n' || event->key == 'p') {
		size_t item = nt_list_view_selected(page->list), step = (size_t)page->page_size;
		size_t next = event->key == 'p' ? (item > step ? item - step : 0) : item + step;
		if (next >= (size_t)page->model.session_count)
			next = (size_t)page->model.session_count - 1;
		return nt_list_view_select(page->list, next);
	}
	return 0;
}

static int cli_resume_select(struct cli_application *ctx, int64_t id)
{
	struct session selected;
	int rc = runtime_session_select_existing(ctx->runtime, id, &selected);

	if (rc) {
		CMD_ERROR(ctx, "failed to resume session: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}
	ctx->session_auto_named = 1;
	rc = cli_session_history_restore(ctx);
	if (rc)
		CMD_ERROR(ctx, "failed to restore session history: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	else
		CMD_OK(ctx, "resumed session: %s", selected.name);
	return rc;
}

static void cli_resume_picker_cleanup(struct cli_resume_picker **owner)
{
	struct cli_resume_picker *picker = owner ? *owner : NULL;
	if (!picker)
		return;
	cli_resume_model_cleanup(&picker->model);
	cli_resume_sessions_cleanup(&picker->sessions);
	free(picker);
	*owner = NULL;
}

static void cli_resume_close(void *data, int result)
{
	struct cli_resume_picker *picker = data;
	struct cli_application *ctx = picker->ctx;

	if (!result && picker->selection >= 0)
		(void)cli_resume_select(ctx, picker->model.sessions[picker->selection].id);
	cli_resume_picker_cleanup(&picker);
}

static void cli_resume_deinit(struct nt_view_controller *controller)
{
	struct cli_resume_picker *page = controller->data;
	if (page)
		cli_application_panel_complete(page->ctx, page, cli_resume_close,
					       nt_view_controller_result(controller));
}

static int cli_resume_present_take(struct cli_application *ctx, struct session **sessions,
				   int count)
{
	static const struct nt_view_controller_class type = {
		.superclass = &nt_view_controller_base_class,
		.instance_size = sizeof(struct nt_view_controller),
		.load_view = cli_resume_build,
		.handle_event = cli_resume_event,
		.deinit = cli_resume_deinit,
	};
	struct cli_resume_picker *page NB_CLEANUP(cli_resume_picker_cleanup) =
		calloc(1, sizeof(*page));
	if (!page)
		return NB_ERR_NOMEM;
	page->ctx = ctx;
	page->selection = -1;
	int rc = cli_resume_model_build(&page->model, *sessions, count,
					runtime_workdir_borrow(ctx->runtime));
	if (rc)
		return rc;
	page->sessions = *sessions;
	int accepted = 0;
	rc = cli_application_show_page(ctx, &type, page, 0, &accepted);
	if (accepted) {
		*sessions = NULL;
		page = NULL;
	} else {
		page->sessions = NULL;
	}
	return rc;
}

int cli_resume_show(struct cli_application *ctx)
{
	struct session *list NB_CLEANUP(cli_resume_sessions_cleanup) = NULL;
	struct cli_resume_model model NB_CLEANUP(cli_resume_model_cleanup) = {0};
	int64_t current_id = 0;
	int count = 0;
	int page = 0;
	int rc;

	if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON) {
		CMD_ERROR(ctx, "use /resume <name|id> in JSON mode");
		return NB_ERR_INVAL;
	}
	rc = runtime_session_list_query(ctx->runtime, &list, &count, 0, NULL);
	if (rc != 0)
		return rc;
	if (count == 0) {
		CMD_OK(ctx, "no sessions to resume");
		return 0;
	}
	int controls_available = nt_terminal_available(stdin, stdout);
	if (controls_available)
		return cli_resume_present_take(ctx, &list, count);
	rc = cli_resume_model_build(&model, list, count, runtime_workdir_borrow(ctx->runtime));
	if (rc != NB_OK)
		return rc;
	(void)runtime_session_current_id(ctx->runtime, &current_id);
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
	return rc;
}
