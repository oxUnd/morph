#include "sapi/cli/components/agents_browser.h"

#include <nothing/tui/keymap.h>

#include "sapi/cli/components/shortcut_hint.h"

#include "sapi/cli/presentation/agents.h"
#include "sapi/cli/presentation/style.h"
#include <nb/buf.h>
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/utf8.h>
#include <nothing/tui/binding.h>
#include <nothing/tui/component_state.h>
#include <nothing/tui/controls/list_view.h>
#include <nothing/tui/controls/tab_bar.h>
#include <nothing/tui/input.h>
#include <nothing/tui/terminal/text.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/scroll_view.h>
#include <nothing/tui/views/text_view.h>
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

static enum cli_agents_browser_filter
cli_agents_browser_group(const struct cli_agents_browser_data *data, size_t index)
{
	if (!index)
		return data->root_running ? CLI_AGENTS_BROWSER_RUNNING
					  : CLI_AGENTS_BROWSER_INACTIVE;

	switch (data->agents[index - 1].status) {
	case AGENT_PENDING:
		return CLI_AGENTS_BROWSER_PENDING;
	case AGENT_RUNNING:
		return CLI_AGENTS_BROWSER_RUNNING;
	case AGENT_COMPLETED:
		return CLI_AGENTS_BROWSER_COMPLETED;
	case AGENT_FAILED:
		return CLI_AGENTS_BROWSER_FAILED;
	case AGENT_CANCELLED:
		return CLI_AGENTS_BROWSER_INTERRUPTED;
	}

	return CLI_AGENTS_BROWSER_ALL;
}

int cli_agents_browser_set_snapshot(struct cli_agents_browser_state *state,
				    const struct cli_agents_browser_data *data, size_t selected)
{
	if (!state || !data || data->count < 0 || (data->count && !data->agents) ||
	    (unsigned)state->filter >= CLI_AGENTS_BROWSER_FILTER_COUNT)
		return NB_ERR_INVAL;

	size_t count = (size_t)data->count + 1;
	if (!nb_vec_elem_size(&state->rows))
		nb_vec_init(&state->rows, sizeof(selected));

	if (!nb_vec_reserve(&state->rows, count))
		return NB_ERR_NOMEM;

	nb_vec_clear(&state->rows);
	size_t *rows = nb_vec_push_n(&state->rows, count);
	if (!rows)
		return NB_ERR_NOMEM;

	/* All uses the snapshot's order directly. Every other tab owns a
	 * contiguous slice of row indices in that same order. */
	memset(state->counts, 0, sizeof(state->counts));
	state->live_elapsed = 0;
	for (size_t i = 0; i < count; i++) {
		state->counts[cli_agents_browser_group(data, i)]++;
		if (i && data->agents[i - 1].started_at_ms > 0 &&
		    data->agents[i - 1].ended_at_ms <= 0)
			state->live_elapsed = 1;
	}

	state->counts[CLI_AGENTS_BROWSER_ALL] = count;
	size_t next[CLI_AGENTS_BROWSER_FILTER_COUNT] = {0};
	for (int i = CLI_AGENTS_BROWSER_RUNNING + 1; i < CLI_AGENTS_BROWSER_FILTER_COUNT; i++)
		next[i] = next[i - 1] + state->counts[i - 1];

	memcpy(state->offsets, next, sizeof(next));
	if (state->filter == CLI_AGENTS_BROWSER_ALL && selected < count)
		state->selected = selected;

	for (size_t i = 0; i < count; i++) {
		enum cli_agents_browser_filter group = cli_agents_browser_group(data, i);
		if (group == CLI_AGENTS_BROWSER_ALL)
			continue;

		size_t slot = next[group]++;
		rows[slot] = i;
		if (group == state->filter && i == selected)
			state->selected = slot - state->offsets[group];
	}

	if (state->selected >= state->counts[state->filter])
		state->selected =
			state->counts[state->filter] ? state->counts[state->filter] - 1 : 0;
	state->root_running = data->root_running;
	state->revision++;
	return 0;
}

void cli_agents_browser_state_cleanup(struct cli_agents_browser_state *state)
{
	nb_vec_destroy(&state->rows);
	*state = (struct cli_agents_browser_state){0};
}

static size_t cli_agents_browser_index(const struct cli_agents_browser_state *state, size_t row)
{
	if (row >= state->counts[state->filter])
		return SIZE_MAX;

	if (state->filter == CLI_AGENTS_BROWSER_ALL)
		return row;

	return *(const size_t *)nb_vec_at(&state->rows, state->offsets[state->filter] + row);
}

size_t cli_agents_browser_selected(const struct cli_agents_browser_state *state)
{
	return cli_agents_browser_index(state, state->selected);
}

static void cli_agents_browser_set_filter(struct cli_agents_browser_state *state,
					  enum cli_agents_browser_filter filter)
{
	state->filter = filter;
	state->selected = 0;
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

struct cli_agents_browser {
	struct nt_view base;
	struct cli_agents_browser_state *state;
	struct cli_agents_browser_data data;
	struct nt_view *heading, *tabs, *rule, *columns, *list;
	struct nt_view *detail_heading, *detail_scroll, *detail_text, *footer;
	int external_footer;
	struct nt_component_state *agent_count, *hint;
	nb_buf project, model;
	nb_buf row_text, clean_text;
	size_t counts[CLI_AGENTS_BROWSER_FILTER_COUNT];
	uint64_t revision;
	size_t selected;
	enum cli_agents_browser_filter filter;
	int styled, restyle, details_changed, syncing, width, split, body_top, body_rows, divider;
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

static int cli_agents_browser_draw_row(void *data, struct nt_canvas *canvas, size_t item,
				       size_t group, int selected)
{
	(void)group;
	struct cli_agents_browser *browser = data;
	size_t index = cli_agents_browser_index(browser->state, item);
	if (index == SIZE_MAX)
		return 0;

	const struct agent_info *agent = index ? &browser->data.agents[index - 1] : NULL;
	enum agent_status status = cli_agents_browser_status(&browser->data, index);
	struct nt_text_style style = browser->styled
					     ? cli_agents_browser_status_style(status, selected)
					     : (struct nt_text_style){0};
	nb_buf *row = &browser->row_text, *clean = &browser->clean_text;
	nb_buf_clear(row);
	nb_buf_clear(clean);
	if (!nb_buf_printf(row, "%s %s %s  %s", selected ? ">" : " ",
			   cli_agents_browser_marker(status), agent ? agent->id : "/root",
			   agent ? (agent->description ? agent->description : "")
				 : "Main conversation"))
		return nb_buf_status(row);

	struct nt_sanitizer sanitizer;
	nt_sanitizer_init(&sanitizer, NT_TEXT_SINGLE_LINE);
	int rc = nt_sanitize_feed(&sanitizer, clean, row->ptr, row->len, 1);
	if (rc)
		return rc;

	int metadata = canvas->width >= 70 ? 23 : 0;
	nb_buf_clear(row);
	if (!nb_buf_reserve(row, clean->len + sizeof("…")))
		return nb_buf_status(row);

	nt_terminal_copy_ellipsized_display_width(row->ptr, row->cap, nb_buf_cstr(clean),
						  (size_t)(canvas->width - metadata), 0);
	rc = nt_canvas_fill_styled(canvas, &style);
	if (!rc)
		rc = nt_canvas_text_styled(canvas, 0, 0, row->ptr, &style);

	if (rc || !metadata)
		return rc;

	rc = nt_canvas_text_styled(
		canvas, canvas->width - 22, 0,
		!index && !browser->data.root_running ? "inactive" : cli_agents_status(status),
		&style);
	if (rc || !agent || agent->started_at_ms <= 0)
		return rc;

	int64_t end = agent->ended_at_ms > 0 ? agent->ended_at_ms : browser->data.now_ms;
	if (end < agent->started_at_ms)
		return 0;

	char elapsed[32], clipped[32];
	snprintf(elapsed, sizeof(elapsed), "%llds",
		 (long long)((end - agent->started_at_ms) / 1000));
	nt_terminal_copy_ellipsized_display_width(clipped, sizeof(clipped), elapsed, 8, 0);
	return nt_canvas_text_styled(canvas, canvas->width - 8, 0, clipped, &style);
}

static const char *cli_agents_browser_source(void *data, size_t item)
{
	const struct cli_agents_browser *browser = data;
	size_t index = cli_agents_browser_index(browser->state, item);
	if (index == SIZE_MAX)
		return NULL;

	return index ? browser->data.agents[index - 1].id : "/root";
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
		snprintf(labels[i], sizeof(labels[i]), "%s %zu%s",
			 width < 100 ? short_names[i] : names[i], browser->state->counts[i],
			 browser->styled ? "  " : "");
		items[i] = labels[i];
	}

	return nt_tab_bar_set_items(browser->tabs, items, CLI_AGENTS_BROWSER_FILTER_COUNT);
}

static int cli_agents_browser_columns(struct cli_agents_browser *browser, int width, int left)
{
	nb_buf line NB_CLEANUP(nb_buf_cleanup) = {0};
	for (int i = 0; i < width; i++)
		if (!nb_buf_append_cstr(&line, "─"))
			return NB_ERR_NOMEM;

	int rc = nt_label_set_text(browser->rule, nb_buf_cstr(&line));
	if (rc)
		return rc;

	nb_buf_clear(&line);
	char tasks[64];
	nt_terminal_copy_ellipsized_display_width(tasks, sizeof(tasks), "Tasks · current session",
						  (size_t)(left >= 70 ? left - 23 : left), 0);
	nb_buf_append_cstr(&line, tasks);
	if (left >= 70)
		nb_buf_printf(&line, "%*s%-14sElapsed",
			      left - 22 - (int)nb_utf8_width_cstr(nb_buf_cstr(&line)), "",
			      "Status");

	return nb_buf_ok(&line) ? nt_label_set_text(browser->columns, nb_buf_cstr(&line))
				: nb_buf_status(&line);
}

static int cli_agents_browser_heading(void *data, struct nt_value *result)
{
	return nt_value_format(result, "Agents (%d)  Agent command center",
			       (int)nt_component_state_get_int(data));
}

static int cli_agents_browser_footer(void *data, struct nt_value *result)
{
	return nt_value_set_string(result, nt_component_state_get_string(data));
}

static int cli_agents_browser_detail_spans(struct cli_agents_browser *browser, int width,
					   const char *id, enum agent_status status,
					   const char *label, const char *fields)
{
	const char *heading = nt_label_text(browser->detail_heading);
	nb_buf prefix NB_CLEANUP(nb_buf_cleanup) = {0};
	if (id)
		nb_buf_printf(&prefix, "%s\n\n%s\n%s %s\n", heading, id,
			      cli_agents_browser_marker(status), label);
	else
		nb_buf_printf(&prefix, "%s\n\nNo matching agents.\n", heading);

	if (!nb_buf_ok(&prefix))
		return nb_buf_status(&prefix);

	char *safe NB_CLEANUP(nb_charp_cleanup) =
		nt_sanitize_dup(prefix.ptr, prefix.len, NT_TEXT_MULTILINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;

	struct nt_text_layout layout NB_CLEANUP(nt_text_layout_deinit) = {0};
	nt_text_layout_init(&layout);
	int rc = nt_text_layout_build(&layout, safe,
				      (size_t)(width > 0 ? width : 1), NT_TEXT_WRAP_CHAR);
	if (rc)
		return rc;

	/* The first display line stays pinned; the fourth takes the status color. */
	const struct nt_text_line *body = nb_vec_at(&layout.lines, 1);
	struct nt_text_span spans[4] = {{.text = safe + body->offset}, {.text = fields}};
	if (!id || !browser->styled)
		return nt_text_view_set_spans(browser->detail_text, spans, 2);

	const struct nt_text_line *row = nb_vec_at(&layout.lines, 3);
	nb_buf before NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf colored NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!nb_buf_append(&before, spans[0].text, row->offset - body->offset) ||
	    !nb_buf_append(&colored, safe + row->offset, row->length) ||
	    !nb_buf_printf(&colored, "%*s", width > row->width ? width - row->width : 0, ""))
		return NB_ERR_NOMEM;

	spans[0].text = nb_buf_cstr(&before);
	spans[1] = (struct nt_text_span){.text = nb_buf_cstr(&colored),
					 .style = cli_agents_browser_status_style(status, 0)};
	spans[2].text = safe + row->offset + row->length;
	spans[3].text = fields;
	return nt_text_view_set_spans(browser->detail_text, spans, 4);
}

static int cli_agents_browser_set_details(struct cli_agents_browser *browser, int split, int width)
{
	size_t index = cli_agents_browser_selected(browser->state);
	if (index == SIZE_MAX)
		return cli_agents_browser_detail_spans(browser, width, NULL, AGENT_PENDING, NULL, NULL);

	const struct cli_agents_browser_data *data = &browser->data;
	const struct agent_info *agent = index ? &data->agents[index - 1] : NULL;
	if (agent && data->detail && !strcmp(agent->id, data->detail->id))
		agent = data->detail;

	enum agent_status status = cli_agents_browser_status(data, index);
	const char *prompt = agent && agent->description ? agent->description : "Main conversation";
	const char *activity = data->root_running ? "Working" : "Idle";
	if (agent)
		activity = agent->activity ? agent->activity : cli_agents_status(agent->status);

	nb_buf fields NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!split)
		nb_buf_printf(&fields, "\nPrompt\n%s\n", prompt);

	nb_buf_printf(&fields, "\nSession model\n%s\n\nProject\n%s\n\nActivity\n%s\n", data->model,
		      data->project, activity);
	if (split)
		nb_buf_printf(&fields, "\nPrompt\n%s\n", prompt);

	nb_buf_printf(&fields, "\nLast message\n%s\n",
		      agent && agent->result ? agent->result : "No result yet.");
	if (!nb_buf_ok(&fields))
		return nb_buf_status(&fields);

	const char *label = !index && !data->root_running ? "inactive" : cli_agents_status(status);
	const char *id = agent ? agent->id : "/root  Main conversation";
	return cli_agents_browser_detail_spans(browser, width, id, status, label, nb_buf_cstr(&fields));
}

static int cli_agents_browser_layout(struct nt_view *view)
{
	struct cli_agents_browser *b = (struct cli_agents_browser *)view;
	struct nt_view_rect r = nt_view_content_rect(view);
	if (!b->state)
		return 0;

	int w = r.width, h = r.height;
	int header = h >= 8 ? 4 : h > 0;
	int footer = !b->external_footer && h >= 3;
	int body = h - header - footer;
	int split = w >= 100 && body >= 4;
	int left = split ? w * 3 / 5 : w;
	int right = split ? w - left - 3 : w;
	size_t count = b->state->counts[b->state->filter];
	int list_rows = !split && body >= 8 ? body - body / 2 : body;
	if (!split && body >= 8 && count < (size_t)list_rows)
		list_rows = count ? (int)count : 1;

	int detail_rows = split ? body : body - list_rows;
	int detail_top = split ? header : header + list_rows;
	int detail_x = split ? left + 3 : 0;
	int geometry = w != b->width || split != b->split;
	int snapshot = b->revision != b->state->revision;
	int tabs = (w < 100) != (b->width < 100) ||
		   memcmp(b->counts, b->state->counts, sizeof(b->counts));
	size_t selected = cli_agents_browser_selected(b->state);
	int rc = 0;
	if (geometry || !b->revision)
		rc = cli_agents_browser_columns(b, w, left);

	b->syncing = 1;
	if (!rc && (tabs || b->restyle))
		rc = cli_agents_browser_tabs(b, w);

	if (!rc && nt_tab_bar_selected(b->tabs) != (size_t)b->state->filter) {
		rc = nt_tab_bar_select(b->tabs, (size_t)b->state->filter);
		if (rc > 0)
			rc = 0;
	}

	if (!rc &&
	    (snapshot || geometry || b->restyle || b->details_changed || b->selected != selected))
		rc = cli_agents_browser_set_details(b, split, right);

	if (!rc && nt_list_view_count(b->list) != count)
		rc = nt_list_view_set_source(b->list, count, cli_agents_browser_source, b);

	if (!rc && count && nt_list_view_selected(b->list) != b->state->selected)
		rc = nt_list_view_select(b->list, b->state->selected);

	b->syncing = 0;
	if (rc)
		return rc;

	b->split = split;
	b->width = w;
	b->body_top = header;
	b->body_rows = body;
	b->divider = left + 1;
	b->revision = b->state->revision;
	b->filter = b->state->filter;
	b->selected = selected;
	b->restyle = 0;
	b->details_changed = 0;
	memcpy(b->counts, b->state->counts, sizeof(b->counts));

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
	size_t child_count = sizeof(children) / sizeof(children[0]) - (size_t)b->external_footer;
	for (size_t i = 0; !rc && i < child_count; i++) {
		slots[i].x += r.x;
		slots[i].y += r.y;
		rc = nt_view_arrange(children[i], slots[i]);
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

static int cli_agents_browser_hint(struct cli_agents_browser *browser)
{
	if (!browser->state)
		return 0;

	const char *hint =
		browser->state->help ? "Esc back · Enter open · Tab/Shift+Tab filter · Left/Right "
				       "focus · PageUp/PageDown scroll"
		: nt_view_is_focused(browser->detail_scroll)
			? "Esc back · ↑/↓ scroll details · Left tasks · Enter open · ? help"
			: "Esc back · ↑/↓ move · Enter open · Tab filter · ? help";
	int rc = nt_component_state_set_string(browser->hint, hint);
	return rc;
}

int cli_agents_browser_focus(struct nt_view *view, int details)
{
	struct cli_agents_browser *browser = (struct cli_agents_browser *)view;
	return nt_view_focus(details ? browser->detail_scroll : browser->list);
}

static int cli_agents_browser_capture(struct nt_view *view, const struct nt_event *event)
{
	struct cli_agents_browser *browser = (struct cli_agents_browser *)view;
	if (!browser->state || event->type != NT_EVENT_KEY || event->pasting)
		return 0;

	int rc = 0;
	if (event->key == '\t' || event->key == NT_KEY_BACK_TAB) {
		int previous = (int)browser->state->filter;
		int delta = event->key == '\t' ? 1 : CLI_AGENTS_BROWSER_FILTER_COUNT - 1;
		size_t next = (size_t)((previous + delta) % CLI_AGENTS_BROWSER_FILTER_COUNT);
		rc = nt_tab_bar_select(browser->tabs, next);
		return rc < 0 ? rc : NT_VIEW_EVENT_STOP;
	}

	if (!nt_view_is_focused(browser->tabs) &&
	    (event->key == NT_KEY_LEFT || event->key == NT_KEY_RIGHT)) {
		rc = cli_agents_browser_focus(view, event->key == NT_KEY_RIGHT);
		return rc ? rc : NT_VIEW_EVENT_STOP;
	}

	if (event->key == '?') {
		browser->state->help = !browser->state->help;
		rc = cli_agents_browser_hint(browser);
		return rc ? rc : NT_VIEW_EVENT_STOP;
	}

	if ((event->key == '\r' || event->key == '\n') &&
	    nt_view_is_focused(browser->detail_scroll)) {
		size_t selected = cli_agents_browser_selected(browser->state);
		if (selected != SIZE_MAX)
			return nt_view_emit(view, NT_VIEW_EVENT_ACTIVATE, selected, NULL);
	}

	return 0;
}

static int cli_agents_browser_control_changed(void *data, const struct nt_view_event *event)
{
	struct cli_agents_browser *browser = data;
	struct cli_agents_browser_state *state = browser->state;
	if (browser->syncing || !state)
		return NT_VIEW_EVENT_PASS;

	if (event->type == NT_VIEW_EVENT_FOCUS_GAINED)
		return cli_agents_browser_hint(browser);

	if (event->type == NT_VIEW_EVENT_ACTIVATE && event->target == browser->list) {
		size_t selected = cli_agents_browser_selected(state);
		return selected == SIZE_MAX ? 0
					    : nt_view_emit(&browser->base, NT_VIEW_EVENT_ACTIVATE,
							   selected, NULL);
	}

	if (event->target == browser->tabs && event->index < CLI_AGENTS_BROWSER_FILTER_COUNT)
		cli_agents_browser_set_filter(state, (enum cli_agents_browser_filter)event->index);
	else if (event->target == browser->list && event->index < state->counts[state->filter])
		state->selected = event->index;
	else
		return NT_VIEW_EVENT_PASS;

	int rc = nt_scroll_view_set_offset(browser->detail_scroll, 0);
	nt_view_invalidate(&browser->base, 1);
	return rc < 0 ? rc
		      : nt_view_emit(&browser->base, NT_VIEW_EVENT_SELECTION_CHANGED,
				     cli_agents_browser_selected(state), NULL);
}

static void cli_agents_browser_destroy(struct nt_view *view)
{
	struct cli_agents_browser *browser = (struct cli_agents_browser *)view;
	nb_buf_destroy(&browser->row_text);
	nb_buf_destroy(&browser->clean_text);
	nb_buf_destroy(&browser->project);
	nb_buf_destroy(&browser->model);
}

static const struct nt_keymap *cli_agents_browser_keymap(const struct nt_view *view)
{
	(void)view;
	static const struct nt_key_binding bindings[] = {
		{.key = '\t', .description = "Next agent filter"},
		{.key = '\t', .modifiers = NT_MOD_SHIFT, .description = "Previous agent filter"},
		{.key = NT_KEY_LEFT, .description = "Focus agent list"},
		{.key = NT_KEY_RIGHT, .description = "Focus agent details"},
		{.key = '?', .description = "Toggle agent browser help"},
	};
	static const struct nt_keymap map = {bindings, NT_ARRAY_SIZE(bindings)};
	return &map;
}

struct nt_view *cli_agents_browser_add(struct nt_builder *builder, struct nt_view *parent,
				      struct nt_view *footer)
{
	static const struct nt_view_class type = {
		.superclass = &nt_view_base_class,
		.kind = "agents_browser",
		.instance_size = sizeof(struct cli_agents_browser),
		.layout = cli_agents_browser_layout,
		.draw = cli_agents_browser_draw,
		.capture_event = cli_agents_browser_capture,
		.keymap = cli_agents_browser_keymap,
		.deinit = cli_agents_browser_destroy,
	};
	struct cli_agents_browser *b =
		(struct cli_agents_browser *)nt_view_add(builder, parent, &type, NULL);
	if (!b)
		return NULL;

	struct nt_view *v = &b->base;
	nt_builder_fail(builder, nt_view_set_help_label(v, "Agents"));
	nt_builder_fail(builder, nt_view_use_state_int(v, 0, &b->agent_count));
	nt_builder_fail(builder, nt_view_use_state_string(v, "", &b->hint));
	b->heading = nt_label_add(builder, v, "", NULL);
	b->tabs = nt_tab_bar_add(builder, v, NULL, 0, 0, 0, NULL);
	b->rule = nt_label_add(builder, v, "", NULL);
	b->columns = nt_label_add(builder, v, "", NULL);
	b->list = nt_list_view_add(builder, v, NULL, 0, 0, NULL);
	b->detail_heading = nt_label_add(builder, v, "Task details", NULL);
	b->detail_scroll = nt_scroll_view_add(builder, v, NULL);
	b->detail_text = nt_text_view_add(builder, b->detail_scroll, "", NULL);
	b->external_footer = footer != NULL;
	b->footer = cli_shortcut_hint_add(builder, footer ? footer : v, "",
				 &(struct nt_view_options){.height = 1});
	nt_builder_fail(builder, nt_view_bind_compute(b->heading, NT_PROP_TEXT,
						      cli_agents_browser_heading, b->agent_count));
	nt_builder_fail(builder, nt_view_bind_compute(b->footer, NT_PROP_TEXT,
						      cli_agents_browser_footer, b->hint));
	struct nt_view *labels[] = {b->heading, b->rule, b->columns, b->detail_heading};
	for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++)
		nt_builder_fail(builder, nt_label_set_wrap_mode(labels[i], NT_TEXT_WRAP_NONE));

	nt_builder_fail(builder, nt_label_set_opaque(b->columns, 1));
	nt_builder_fail(builder, nt_label_set_opaque(b->detail_heading, 1));
	nt_builder_fail(builder,
			nt_list_view_set_row_renderer(b->list, cli_agents_browser_draw_row, b));
	nt_builder_fail(builder, nt_scroll_view_set_scrollbars(b->detail_scroll, 0));
	nt_builder_fail(builder, nt_text_view_set_wrap_mode(b->detail_text, NT_TEXT_WRAP_CHAR));
	if (!nt_view_on(b->list, NT_VIEW_EVENT_CHANGED, cli_agents_browser_control_changed, b) ||
	    !nt_view_on(b->tabs, NT_VIEW_EVENT_CHANGED, cli_agents_browser_control_changed, b) ||
	    !nt_view_on(b->list, NT_VIEW_EVENT_ACTIVATE, cli_agents_browser_control_changed, b) ||
	    !nt_view_on(v, NT_VIEW_EVENT_FOCUS_GAINED, cli_agents_browser_control_changed, b))
		nt_builder_fail(builder, NB_ERR_NOMEM);

	nt_builder_fail(builder, nt_view_set_focusable(b->detail_text, 0));
	nt_builder_fail(builder, nt_view_set_focusable(b->detail_scroll, 1));
	nt_builder_fail(builder, nt_list_view_set_navigation(b->list, NT_LIST_VIEW_VIM));

	return v;
}

int cli_agents_browser_sync(struct nt_view *view, struct cli_agents_browser_state *state,
			    const struct cli_agents_browser_data *data, int styled)
{
	struct cli_agents_browser *b = (struct cli_agents_browser *)view;
	if (!b || !state || !data || data->count < 0 || (data->count && !data->agents))
		return NB_ERR_INVAL;

	int style_changed = !b->state || b->styled != styled;
	int clock_changed = b->data.now_ms != data->now_ms;
	const char *project = data->project ? data->project : "";
	const char *model = data->model ? data->model : "";
	if (strcmp(nb_buf_cstr(&b->project), project) || strcmp(nb_buf_cstr(&b->model), model)) {
		b->details_changed = 1;
		nb_buf_clear(&b->project);
		nb_buf_clear(&b->model);
		if (!nb_buf_append_cstr(&b->project, project) ||
		    !nb_buf_append_cstr(&b->model, model))
			return NB_ERR_NOMEM;
	}

	int rc = 0;
	if (b->data.count != data->count)
		rc = nt_component_state_set_int(b->agent_count, data->count);

	if (!rc) {
		b->state = state;
		rc = cli_agents_browser_hint(b);
	}

	if (rc)
		return rc;

	b->state = state;
	b->data = *data;
	b->data.project = nb_buf_cstr(&b->project);
	b->data.model = nb_buf_cstr(&b->model);
	b->styled = styled;

	if (style_changed) {
		b->restyle = 1;
		struct nt_text_style dim = {.attributes = styled ? NT_STYLE_DIM : 0};
		struct nt_text_style bold = {.attributes = styled ? NT_STYLE_BOLD : 0};
		struct nt_text_style tab =
			styled ? cli_agents_browser_status_style(AGENT_RUNNING, 1)
			       : (struct nt_text_style){0};
		rc = nt_view_set_style(b->columns, dim);
		if (!rc)
			rc = nt_view_set_style(b->detail_heading, bold);

		if (!rc)
			rc = nt_tab_bar_set_appearance(b->tabs, styled ? 0u : 2u, !styled, tab);
	}

	if (b->restyle || b->details_changed || b->revision != state->revision ||
	    b->filter != state->filter || b->selected != cli_agents_browser_selected(state))
		nt_view_invalidate(view, 1);
	else if (clock_changed && b->divider > 70 && state->live_elapsed)
		nt_view_invalidate(b->list, 0);

	return rc;
}
