#include "sapi/cli/components/agents_browser.h"
#include <nb/err.h>

#include "sapi/cli/presentation/style.h"
#include "sapi/cli/components/agents.h"
#include <nothing/tui/controls/list_view.h>
#include <nothing/tui/controls/tab_bar.h>
#include <nothing/tui/views/text_view.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/scroll_view.h>
#include <nb/cleanup.h>
#include <nb/utf8.h>
#include <nothing/tui/terminal/text.h>
#include <nothing/tui/text.h>
#include <nothing/tui/input.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static enum agent_status cli_agents_browser_status(const struct cli_agents_browser_data *data,
						   size_t index)
{
	return index		    ? data->agents[index - 1].status
	       : data->root_running ? AGENT_RUNNING
				    : AGENT_PENDING;
}

static int cli_agents_browser_matches(enum cli_agents_browser_filter filter,
				      enum agent_status status)
{
	switch (filter) {
	case CLI_AGENTS_BROWSER_ALL:
		return 1;
	case CLI_AGENTS_BROWSER_RUNNING:
		return status == AGENT_RUNNING;
	case CLI_AGENTS_BROWSER_PENDING:
		return status == AGENT_PENDING;
	case CLI_AGENTS_BROWSER_COMPLETED:
		return status == AGENT_COMPLETED;
	case CLI_AGENTS_BROWSER_FAILED:
		return status == AGENT_FAILED;
	case CLI_AGENTS_BROWSER_INTERRUPTED:
		return status == AGENT_CANCELLED;
	default:
		return 0;
	}
}

static int cli_agents_browser_entry_matches(enum cli_agents_browser_filter filter,
					    const struct cli_agents_browser_data *data,
					    size_t index)
{
	if (!index && !data->root_running)
		return filter == CLI_AGENTS_BROWSER_ALL || filter == CLI_AGENTS_BROWSER_INACTIVE;
	return cli_agents_browser_matches(filter, cli_agents_browser_status(data, index));
}

static size_t cli_agents_browser_count(const struct cli_agents_browser_data *data,
				       enum cli_agents_browser_filter filter)
{
	size_t count = 0;
	for (size_t i = 0; i <= (size_t)data->count; i++)
		count += (size_t)cli_agents_browser_entry_matches(filter, data, i);
	return count;
}

size_t cli_agents_browser_selected(const struct cli_agents_browser_state *state,
				   const struct cli_agents_browser_data *data)
{
	size_t visible = 0;
	for (size_t i = 0; i <= (size_t)data->count; i++) {
		if (!cli_agents_browser_entry_matches(state->filter, data, i))
			continue;
		if (visible++ == state->selection.index)
			return i;
	}
	return SIZE_MAX;
}

void cli_agents_browser_select(struct cli_agents_browser_state *state,
			       const struct cli_agents_browser_data *data, size_t index)
{
	size_t visible = 0;
	for (size_t i = 0; i <= (size_t)data->count; i++) {
		if (!cli_agents_browser_entry_matches(state->filter, data, i))
			continue;
		if (i == index) {
			state->selection.index = visible;
			return;
		}
		visible++;
	}
	nt_list_state_layout(&state->selection, visible,
			     state->selection.page ? state->selection.page : 1);
}

int cli_agents_browser_update(struct cli_agents_browser_state *state,
			      const struct cli_agents_browser_data *data, int key)
{
	if (key == 27 || key == 3 || key == 4 || key == 'q')
		return NT_NAVIGATION_CANCEL;
	if (key == '\r' || key == '\n')
		return cli_agents_browser_selected(state, data) == SIZE_MAX
			       ? NT_NAVIGATION_UNHANDLED
			       : NT_NAVIGATION_SUBMIT;
	if (key == '\t' || key == NT_KEY_BACK_TAB) {
		int delta = key == '\t' ? 1 : CLI_AGENTS_BROWSER_FILTER_COUNT - 1;
		state->filter = (enum cli_agents_browser_filter)(((int)state->filter + delta) %
								 CLI_AGENTS_BROWSER_FILTER_COUNT);
		state->selection.index = 0;
		state->selection.first = 0;
		state->detail_scroll = 0;
	} else if (key == '?') {
		state->help = !state->help;
	} else if (key == NT_KEY_RIGHT || key == NT_KEY_LEFT) {
		state->detail_focus = key == NT_KEY_RIGHT;
	} else if (state->detail_focus) {
		if (key == NT_KEY_DOWN || key == NT_KEY_PAGE_DOWN)
			state->detail_scroll += key == NT_KEY_DOWN ? 1u : 8u;
		else if (key == NT_KEY_UP || key == NT_KEY_PAGE_UP) {
			size_t delta = key == NT_KEY_UP ? 1u : 8u;
			state->detail_scroll =
				state->detail_scroll > delta ? state->detail_scroll - delta : 0;
		} else if (key == NT_KEY_HOME)
			state->detail_scroll = 0;
	} else {
		nt_list_state_layout(&state->selection,
				     cli_agents_browser_count(data, state->filter),
				     state->selection.page ? state->selection.page : 1);
		if (nt_list_state_update(&state->selection, key))
			state->detail_scroll = 0;
	}
	return NT_NAVIGATION_CHANGED;
}

static const char *cli_agents_browser_marker(enum agent_status status)
{
	switch (status) {
	case AGENT_RUNNING:
		return CLI_FEED_RUNNING;
	case AGENT_PENDING:
		return CLI_FEED_PENDING;
	case AGENT_COMPLETED:
		return CLI_FEED_DONE;
	case AGENT_FAILED:
		return CLI_FEED_FAILED;
	case AGENT_CANCELLED:
		return CLI_FEED_CANCELLED;
	}
	return CLI_FEED_PENDING;
}

static int cli_agents_browser_field(nb_buf *out, const char *label, const char *value)
{
	return (nb_buf_printf(out, "\n%s\n%s\n", label, value ? value : "") ? 0 : NB_ERR_NOMEM);
}

static int cli_agents_browser_detail(nb_buf *out, const struct cli_agents_browser_data *data,
				     size_t index, int compact)
{
	if (index == SIZE_MAX)
		return (nb_buf_append_cstr(out, "Task details\n\nNo matching agents.\n")
				? 0
				: NB_ERR_NOMEM);
	const struct agent_info *agent = index ? &data->agents[index - 1] : NULL;
	if (agent && data->detail && !strcmp(agent->id, data->detail->id))
		agent = data->detail;
	enum agent_status status = cli_agents_browser_status(data, index);
	const char *prompt = agent && agent->description ? agent->description : "Main conversation";
	int rc = (nb_buf_printf(out, "Task details\n\n%s\n%s %s\n",
				agent ? agent->id : "/root  Main conversation",
				cli_agents_browser_marker(status),
				!index && !data->root_running ? "inactive"
							      : cli_agents_status(status))
			  ? 0
			  : NB_ERR_NOMEM);
	if (rc == 0 && compact)
		rc = cli_agents_browser_field(out, "Prompt", prompt);
	if (rc == 0)
		rc = cli_agents_browser_field(out, "Session model", data->model);
	if (rc == 0)
		rc = cli_agents_browser_field(out, "Project", data->project);
	if (rc == 0)
		rc = cli_agents_browser_field(
			out, "Activity",
			agent ? agent->activity ? agent->activity : cli_agents_status(agent->status)
			: data->root_running ? "Working"
					     : "Idle");
	if (rc == 0 && !compact)
		rc = cli_agents_browser_field(out, "Prompt", prompt);
	if (rc == 0)
		rc = cli_agents_browser_field(out, "Last message",
					      agent && agent->result ? agent->result
								     : "No result yet.");
	return rc;
}

struct cli_agents_browser {
	struct nt_view base;
	struct cli_agents_browser_state *state;
	struct cli_agents_browser_data data;
	struct nt_view *heading, *tabs, *rule, *columns, *list;
	struct nt_view *detail_heading, *detail_scroll, *detail_text, *footer;
	int styled, dirty, width, height, split, body_top, body_rows, divider;
};

static struct nt_text_style cli_agents_browser_status_style(enum agent_status status, int selected)
{
	if (selected)
		return (struct nt_text_style){.attributes = NT_STYLE_BOLD,
					      .foreground = {NT_COLOR_ANSI, 6}};
	if (status == AGENT_FAILED)
		return (struct nt_text_style){.attributes = NT_STYLE_BOLD,
					      .foreground = {NT_COLOR_ANSI, 1}};
	if (status == AGENT_RUNNING || status == AGENT_COMPLETED)
		return (struct nt_text_style){
			.foreground = {NT_COLOR_ANSI, status == AGENT_RUNNING ? 6u : 2u}};
	return (struct nt_text_style){.attributes = NT_STYLE_DIM};
}

static int cli_agents_browser_cell(struct nt_canvas *canvas, int x, int width, const char *text,
				   const struct nt_text_style *style)
{
	char *safe NB_CLEANUP(nb_charp_cleanup) =
		nt_sanitize_dup(text, strlen(text), NT_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;
	if (width <= 0)
		return 0;
	if (nb_utf8_width_cstr(safe) <= (size_t)width)
		return nt_canvas_text_styled(canvas, x, 0, safe, style);
	const char *end = nt_terminal_advance_display_width(safe, (size_t)(width - 1));
	nb_buf clipped NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!nb_buf_append(&clipped, safe, (size_t)(end - safe)) ||
	    !nb_buf_append_cstr(&clipped, "…"))
		return NB_ERR_NOMEM;
	return nt_canvas_text_styled(canvas, x, 0, nb_buf_cstr(&clipped), style);
}

static int cli_agents_browser_draw_row(void *data, struct nt_canvas *canvas, size_t item,
				       size_t group, int selected)
{
	(void)group;
	struct cli_agents_browser *browser = data;
	struct cli_agents_browser_state cursor = *browser->state;
	cursor.selection.index = item;
	size_t index = cli_agents_browser_selected(&cursor, &browser->data);
	if (index == SIZE_MAX)
		return 0;
	const struct agent_info *agent = index ? &browser->data.agents[index - 1] : NULL;
	enum agent_status status = cli_agents_browser_status(&browser->data, index);
	struct nt_text_style style = browser->styled
					     ? cli_agents_browser_status_style(status, selected)
					     : (struct nt_text_style){0};
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!nb_buf_printf(&text, "%s %s %s  %s", selected ? ">" : " ",
			   cli_agents_browser_marker(status), agent ? agent->id : "/root",
			   agent ? (agent->description ? agent->description : "")
				 : "Main conversation"))
		return NB_ERR_NOMEM;
	int metadata = canvas->width >= 70 ? 23 : 0;
	int rc = nt_canvas_fill_styled(canvas, &style);
	if (!rc)
		rc = cli_agents_browser_cell(canvas, 0, canvas->width - metadata,
					     nb_buf_cstr(&text), &style);
	if (!rc && metadata)
		rc = cli_agents_browser_cell(canvas, canvas->width - 22, 14,
					     !index && !browser->data.root_running
						     ? "inactive"
						     : cli_agents_status(status),
					     &style);
	if (!rc && metadata && agent && agent->started_at_ms > 0) {
		int64_t end = agent->ended_at_ms > 0 ? agent->ended_at_ms : browser->data.now_ms;
		if (end >= agent->started_at_ms) {
			char elapsed[32];
			snprintf(elapsed, sizeof(elapsed), "%llds",
				 (long long)((end - agent->started_at_ms) / 1000));
			rc = cli_agents_browser_cell(canvas, canvas->width - 8, 8, elapsed, &style);
		}
	}
	return rc;
}

static const char *cli_agents_browser_source(void *data, size_t item)
{
	(void)data;
	(void)item;
	return "";
}

static int cli_agents_browser_tabs(struct cli_agents_browser *browser, int width)
{
	static const char *const names[] = {"All",    "Running",     "Pending", "Completed",
					    "Failed", "Interrupted", "Inactive"};
	static const char *const short_names[] = {"All",  "Run",  "Wait", "Done",
						  "Fail", "Stop", "Idle"};
	char labels[CLI_AGENTS_BROWSER_FILTER_COUNT][48];
	const char *items[CLI_AGENTS_BROWSER_FILTER_COUNT];
	for (int i = 0; i < CLI_AGENTS_BROWSER_FILTER_COUNT; i++) {
		snprintf(
			labels[i], sizeof(labels[i]), "%s %zu%s",
			width < 100 ? short_names[i] : names[i],
			cli_agents_browser_count(&browser->data, (enum cli_agents_browser_filter)i),
			browser->styled ? "  " : "");
		items[i] = labels[i];
	}
	struct nt_text_style selected = browser->styled
						? cli_agents_browser_status_style(AGENT_RUNNING, 1)
						: (struct nt_text_style){0};
	int rc = nt_tab_bar_set_appearance(browser->tabs, browser->styled ? 0u : 2u,
					   !browser->styled, selected);
	if (!rc)
		rc = nt_tab_bar_set_items(browser->tabs, items, CLI_AGENTS_BROWSER_FILTER_COUNT);
	if (!rc)
		rc = nt_tab_bar_select(browser->tabs, (size_t)browser->state->filter);
	return rc < 0 ? rc : 0;
}

static int cli_agents_browser_set_labels(struct cli_agents_browser *browser, int width, int left)
{
	nb_buf line NB_CLEANUP(nb_buf_cleanup) = {0};
	struct cli_agents_browser_state *state = browser->state;
	if (!nb_buf_printf(&line, "Agents (%d)  Agent command center", browser->data.count))
		return NB_ERR_NOMEM;
	int rc = nt_label_set_text(browser->heading, nb_buf_cstr(&line));
	nb_buf_clear(&line);
	if (!rc)
		rc = cli_agents_browser_tabs(browser, width);
	nb_buf_clear(&line);
	for (int i = 0; i < width; i++)
		if (!nb_buf_append_cstr(&line, "─"))
			return NB_ERR_NOMEM;
	if (!rc)
		rc = nt_label_set_text(browser->rule, nb_buf_cstr(&line));
	nb_buf_clear(&line);
	char tasks[64];
	nt_terminal_copy_ellipsized_display_width(tasks, sizeof(tasks), "Tasks · current session",
						  (size_t)(left >= 70 ? left - 23 : left), 0);
	nb_buf_append_cstr(&line, tasks);
	if (left >= 70)
		nb_buf_printf(&line, "%*s%-14sElapsed",
			      left - 22 - (int)nb_utf8_width_cstr(nb_buf_cstr(&line)), "",
			      "Status");
	if (!rc)
		rc = nb_buf_ok(&line) ? nt_label_set_text(browser->columns, nb_buf_cstr(&line))
				      : NB_ERR_NOMEM;
	if (!rc)
		rc = nt_label_set_text(
			browser->footer,
			state->help ? "Esc back · Enter open · Tab/Shift+Tab filter · Left/Right "
				      "focus · PageUp/PageDown scroll"
			: state->detail_focus
				? "Esc back · ↑/↓ scroll details · Left tasks · Enter open · ? help"
				: "Esc back · ↑/↓ move · Enter open · Tab filter · ? help");
	return rc;
}

static int cli_agents_browser_set_details(struct cli_agents_browser *browser, int split, int width)
{
	nb_buf detail NB_CLEANUP(nb_buf_cleanup) = {0};
	size_t index = cli_agents_browser_selected(browser->state, &browser->data);
	int rc = cli_agents_browser_detail(&detail, &browser->data, index, !split);
	if (rc)
		return rc;
	char *safe NB_CLEANUP(nb_charp_cleanup) =
		nt_sanitize_dup(detail.ptr, detail.len, NT_TEXT_MULTILINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;
	char *body = strchr(safe, '\n');
	if (body)
		*body++ = 0;
	nb_buf heading NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!nb_buf_printf(&heading, "%-*s", width, safe))
		return NB_ERR_NOMEM;
	rc = nt_label_set_text(browser->detail_heading, nb_buf_cstr(&heading));
	if (!rc) {
		struct nt_text_span spans[3] = {{.text = body ? body : ""}};

		if (body && browser->styled && index != SIZE_MAX) {
			char *id = strchr(body, '\n');
			char *status = id ? strchr(id + 1, '\n') : NULL;
			char *end = status ? strchr(status + 1, '\n') : NULL;
			if (end) {
				*status++ = 0;
				*end++ = 0;
				nb_buf middle NB_CLEANUP(nb_buf_cleanup) = {0};
				int cells = (int)nb_utf8_width_cstr(status);
				if (!nb_buf_printf(&middle, "\n%s%*s\n", status,
						   width > cells ? width - cells : 0, ""))
					return NB_ERR_NOMEM;
				spans[1] = (struct nt_text_span){
					.text = nb_buf_cstr(&middle),
					.style = cli_agents_browser_status_style(
						cli_agents_browser_status(&browser->data, index),
						0)};
				spans[2].text = end;
				return nt_text_view_set_spans(browser->detail_text, spans, 3);
			}
		}
		rc = nt_text_view_set_spans(browser->detail_text, spans, 1);
	}
	return rc;
}

static int cli_agents_browser_layout(struct nt_view *view)
{
	struct cli_agents_browser *b = (struct cli_agents_browser *)view;
	struct nt_view_rect r = nt_view_content_rect(view);
	if (!b->state)
		return 0;
	int w = r.width, h = r.height;
	int header = h >= 8 ? 4 : h > 0;
	int footer = h >= 3;
	int body = h - header - footer;
	int split = w >= 100 && body >= 4;
	int left = split ? w * 3 / 5 : w;
	int right = split ? w - left - 3 : w;
	size_t count = cli_agents_browser_count(&b->data, b->state->filter);
	int list_rows = !split && body >= 8 ? body - body / 2 : body;
	if (!split && body >= 8 && count < (size_t)list_rows)
		list_rows = count ? (int)count : 1;
	int detail_rows = split ? body : body - list_rows;
	int detail_top = split ? header : header + list_rows;
	int detail_x = split ? left + 3 : 0;
	b->split = split;
	b->body_top = header;
	b->body_rows = body;
	b->divider = left + 1;
	int rc = 0;
	if (b->dirty || w != b->width || h != b->height) {
		b->width = w;
		b->height = h;
		b->dirty = 0;
		nt_list_state_layout(&b->state->selection, count,
				     (size_t)(list_rows > 0 ? list_rows : 1));
		rc = cli_agents_browser_set_labels(b, w, left);
		if (!rc)
			rc = cli_agents_browser_set_details(b, split, right);
		if (!rc)
			rc = nt_list_view_set_source(b->list, count, cli_agents_browser_source, b);
		if (!rc && count)
			rc = nt_list_view_select(b->list, b->state->selection.index);
	}
	struct nt_view *children[] = {b->heading, b->tabs,	     b->rule,	       b->columns,
				      b->list,	  b->detail_heading, b->detail_scroll, b->footer};
	struct nt_view_rect slots[] = {
		{0, 0, w, header > 0},
		{0, 1, w, header == 4},
		{0, 2, w, header == 4},
		{0, 3, left, header == 4},
		{0, header, left, list_rows},
		{detail_x, detail_top, right, detail_rows > 0},
		{detail_x, detail_top + 1, right, detail_rows > 1 ? detail_rows - 1 : 0},
		{0, h - footer, w, footer}};
	for (size_t i = 0; !rc && i < sizeof(children) / sizeof(children[0]); i++) {
		slots[i].x += r.x;
		slots[i].y += r.y;
		rc = nt_view_arrange(children[i], slots[i]);
	}
	if (!rc && detail_rows > 1) {
		int offset =
			b->state->detail_scroll > INT_MAX ? INT_MAX : (int)b->state->detail_scroll;
		rc = nt_scroll_view_set_offset(b->detail_scroll, offset);
		if (rc >= 0) {
			b->state->detail_scroll = (size_t)nt_scroll_view_offset(b->detail_scroll);
			rc = 0;
		}
	}
	return rc;
}

static int cli_agents_browser_draw(struct nt_view *view, struct nt_canvas *canvas)
{
	struct cli_agents_browser *b = (struct cli_agents_browser *)view;
	for (int row = 0; b->split && row < b->body_rows; row++) {
		int rc = nt_canvas_text(canvas, b->divider, b->body_top + row, "│", 0);
		if (rc)
			return rc;
	}
	return 0;
}

struct nt_view *cli_agents_browser_add(struct nt_builder *builder, struct nt_view *parent)
{
	static const struct nt_view_class type = {.superclass = &nt_view_base_class,
						  .kind = "agents_browser",
						  .instance_size =
							  sizeof(struct cli_agents_browser),
						  .layout = cli_agents_browser_layout,
						  .draw = cli_agents_browser_draw};
	struct cli_agents_browser *b =
		(struct cli_agents_browser *)nt_view_add(builder, parent, &type, NULL);
	if (!b)
		return NULL;
	struct nt_view *v = &b->base;
	b->heading = nt_label_add(builder, v, "", NULL);
	b->tabs = nt_tab_bar_add(builder, v, NULL, 0, 0, 0, NULL);
	b->rule = nt_label_add(builder, v, "", NULL);
	b->columns = nt_label_add(builder, v, "", NULL);
	b->list = nt_list_view_add(builder, v, NULL, 0, 0, NULL);
	b->detail_heading = nt_label_add(builder, v, "", NULL);
	b->detail_scroll = nt_scroll_view_add(builder, v, NULL);
	b->detail_text = nt_text_view_add(builder, b->detail_scroll, "", NULL);
	b->footer = nt_label_add(builder, v, "", NULL);
	struct nt_view *labels[] = {b->heading, b->rule, b->columns, b->detail_heading, b->footer};
	for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++)
		nt_builder_fail(builder, nt_label_set_wrap_mode(labels[i], NT_TEXT_WRAP_NONE));
	nt_builder_fail(builder,
			nt_list_view_set_row_renderer(b->list, cli_agents_browser_draw_row, b));
	nt_builder_fail(builder, nt_scroll_view_set_scrollbars(b->detail_scroll, 0));
	nt_builder_fail(builder, nt_text_view_set_wrap_mode(b->detail_text, NT_TEXT_WRAP_CHAR));
	struct nt_view *focus[] = {b->list, b->tabs, b->detail_text, b->detail_scroll};
	for (size_t i = 0; i < sizeof(focus) / sizeof(focus[0]); i++)
		nt_builder_fail(builder, nt_view_set_focusable(focus[i], 0));
	return v;
}

int cli_agents_browser_sync(struct nt_view *view, struct cli_agents_browser_state *state,
			    const struct cli_agents_browser_data *data, int styled)
{
	struct cli_agents_browser *b = (struct cli_agents_browser *)view;
	if (!b || !state || !data || data->count < 0 || (data->count && !data->agents))
		return NB_ERR_INVAL;
	b->state = state;
	b->data = *data;
	b->styled = styled;
	b->dirty = 1;
	struct nt_text_style dim = {.attributes = styled ? NT_STYLE_DIM : 0};
	struct nt_text_style bold = {.attributes = styled ? NT_STYLE_BOLD : 0};
	int rc = nt_view_set_style(b->columns, dim);
	if (!rc)
		rc = nt_view_set_style(b->detail_heading, bold);
	nt_view_invalidate(view, 1);
	return rc;
}
