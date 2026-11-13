#include "sapi/cli/widgets/agents_browser.h"

#include "sapi/cli/presentation/style.h"
#include "sapi/cli/widgets/agents.h"
#include "sapi/cli/widgets/composer_panel.h"
#include "util/cleanup.h"
#include "util/utf8.h"
#include <morph/tui/widgets/text_view.h>
#include <errno.h>
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
	morph_tui_widget_list_state_layout(&state->selection, visible,
					   state->selection.page ? state->selection.page : 1);
}

int cli_agents_browser_update(struct cli_agents_browser_state *state,
			      const struct cli_agents_browser_data *data, int key)
{
	if (key == 27 || key == 3 || key == 4 || key == 'q')
		return MORPH_TUI_WIDGET_CANCEL;
	if (key == '\r' || key == '\n')
		return cli_agents_browser_selected(state, data) == SIZE_MAX
			       ? MORPH_TUI_WIDGET_UNHANDLED
			       : MORPH_TUI_WIDGET_SUBMIT;
	if (key == '\t' || key == MORPH_TUI_KEY_BACK_TAB) {
		int delta = key == '\t' ? 1 : CLI_AGENTS_BROWSER_FILTER_COUNT - 1;
		state->filter = (enum cli_agents_browser_filter)(((int)state->filter + delta) %
								 CLI_AGENTS_BROWSER_FILTER_COUNT);
		state->selection.index = 0;
		state->selection.first = 0;
		state->detail_scroll = 0;
	} else if (key == '?') {
		state->help = !state->help;
	} else if (key == MORPH_TUI_KEY_RIGHT || key == MORPH_TUI_KEY_LEFT) {
		state->detail_focus = key == MORPH_TUI_KEY_RIGHT;
	} else if (state->detail_focus) {
		if (key == MORPH_TUI_KEY_DOWN || key == MORPH_TUI_KEY_PAGE_DOWN)
			state->detail_scroll += key == MORPH_TUI_KEY_DOWN ? 1u : 8u;
		else if (key == MORPH_TUI_KEY_UP || key == MORPH_TUI_KEY_PAGE_UP) {
			size_t delta = key == MORPH_TUI_KEY_UP ? 1u : 8u;
			state->detail_scroll =
				state->detail_scroll > delta ? state->detail_scroll - delta : 0;
		} else if (key == MORPH_TUI_KEY_HOME)
			state->detail_scroll = 0;
	} else {
		morph_tui_widget_list_state_layout(
			&state->selection, cli_agents_browser_count(data, state->filter),
			state->selection.page ? state->selection.page : 1);
		if (morph_tui_widget_list_state_update(&state->selection, key))
			state->detail_scroll = 0;
	}
	return MORPH_TUI_WIDGET_CHANGED;
}

static void cli_agents_browser_frame_cleanup(struct cli_composer_panel_frame *frame)
{
	morph_buf_cleanup(&frame->content);
}

static int cli_agents_browser_cell(morph_buf_t *out, const char *text, int width, const char *style,
				   int styled)
{
	char *safe MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(
		text, strlen(text), UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return -ENOMEM;
	morph_buf_t clipped MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_buf_init(&clipped, 128);
	if (rc == 0 && width > 0 && utf8_display_width(safe) > (size_t)width) {
		const char *end = utf8_advance_display_width_borrow(safe, (size_t)(width - 1));
		rc = morph_buf_append(&clipped, safe, (size_t)(end - safe));
		if (rc == 0)
			rc = morph_buf_puts(&clipped, "…");
	} else if (rc == 0)
		rc = morph_buf_puts(&clipped, safe);
	struct cli_composer_panel_frame frame MORPH_CLEANUP(cli_agents_browser_frame_cleanup) = {
		.columns = width,
		.rows = 1,
		.color = styled,
	};
	if (rc == 0)
		rc = morph_buf_init(&frame.content, 128);
	if (rc == 0)
		rc = cli_composer_panel_line(&frame, morph_buf_cstr_borrow(&clipped), style);
	if (rc == 0 && frame.content.len)
		rc = morph_buf_append(out, frame.content.data, frame.content.len - 1);
	return rc;
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

static const char *cli_agents_browser_status_style(enum agent_status status)
{
	return status == AGENT_RUNNING	   ? ANSI_CYAN
	       : status == AGENT_COMPLETED ? ANSI_GREEN
	       : status == AGENT_FAILED	   ? CLI_FEED_FAILED_STYLE
					   : ANSI_DIM;
}

static int cli_agents_browser_list_row(morph_buf_t *out, const struct cli_agents_browser_data *data,
				       size_t index, int active, int width, int styled)
{
	const struct agent_info *agent =
		index && index != SIZE_MAX ? &data->agents[index - 1] : NULL;
	if (agent && data->detail && !strcmp(agent->id, data->detail->id))
		agent = data->detail;
	const char *style =
		active && styled ? ANSI_CYAN ANSI_BOLD
		: index == SIZE_MAX
			? ""
			: cli_agents_browser_status_style(cli_agents_browser_status(data, index));
	morph_buf_t label MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_buf_init(&label, 128);
	if (rc == 0 && index != SIZE_MAX)
		rc = morph_buf_printf(
			&label, "%s %s %s%s%s", active ? ">" : " ",
			cli_agents_browser_marker(cli_agents_browser_status(data, index)),
			agent ? agent->id : "/root", agent ? "  " : "  Main conversation",
			agent && agent->description ? agent->description : "");
	int metadata = width >= 70 ? 23 : 0;
	if (rc == 0)
		rc = cli_agents_browser_cell(out, morph_buf_cstr_borrow(&label), width - metadata,
					     style, styled);
	if (rc == 0 && metadata) {
		const char *status =
			index == SIZE_MAX ? ""
			: !index && !data->root_running
				? "inactive"
				: cli_agents_status(cli_agents_browser_status(data, index));
		rc = cli_agents_browser_cell(out, "", 1, style, styled);
		if (rc == 0)
			rc = cli_agents_browser_cell(out, status, 14, style, styled);
		morph_buf_reset(&label);
		if (agent && agent->started_at_ms > 0) {
			int64_t end = agent->ended_at_ms > 0 ? agent->ended_at_ms : data->now_ms;
			if (end >= agent->started_at_ms)
				rc = morph_buf_printf(
					&label, "%llds",
					(long long)((end - agent->started_at_ms) / 1000));
		}
		if (rc == 0)
			rc = cli_agents_browser_cell(out, morph_buf_cstr_borrow(&label), 8, style,
						     styled);
	}
	return rc;
}

static int cli_agents_browser_field(morph_buf_t *out, const char *label, const char *value)
{
	return morph_buf_printf(out, "\n%s\n%s\n", label, value ? value : "");
}

static int cli_agents_browser_detail(morph_buf_t *out, const struct cli_agents_browser_data *data,
				     size_t index, int compact)
{
	if (index == SIZE_MAX)
		return morph_buf_puts(out, "Task details\n\nNo matching agents.\n");
	const struct agent_info *agent = index ? &data->agents[index - 1] : NULL;
	if (agent && data->detail && !strcmp(agent->id, data->detail->id))
		agent = data->detail;
	enum agent_status status = cli_agents_browser_status(data, index);
	const char *prompt = agent && agent->description ? agent->description : "Main conversation";
	int rc = morph_buf_printf(
		out, "Task details\n\n%s\n%s %s\n", agent ? agent->id : "/root  Main conversation",
		cli_agents_browser_marker(status),
		!index && !data->root_running ? "inactive" : cli_agents_status(status));
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

static int cli_agents_browser_tabs(morph_buf_t *out, const struct cli_agents_browser_state *state,
				   const struct cli_agents_browser_data *data, int columns,
				   int styled)
{
	const char *names[] = {"All",	 "Running",	"Pending", "Completed",
			       "Failed", "Interrupted", "Inactive"};
	const char *short_names[] = {"All", "Run", "Wait", "Done", "Fail", "Stop", "Idle"};
	for (int i = 0; i < CLI_AGENTS_BROWSER_FILTER_COUNT; i++) {
		if (styled && state->filter == (enum cli_agents_browser_filter)i)
			(void)morph_buf_puts(out, ANSI_CYAN ANSI_BOLD);
		(void)morph_buf_printf(
			out, "%s%s %zu%s  ",
			!styled && state->filter == (enum cli_agents_browser_filter)i ? "[" : "",
			columns < 100 ? short_names[i] : names[i],
			cli_agents_browser_count(data, (enum cli_agents_browser_filter)i),
			!styled && state->filter == (enum cli_agents_browser_filter)i ? "]" : "");
		if (styled)
			(void)morph_buf_puts(out, ANSI_RESET);
	}
	return out->failed;
}

int cli_agents_browser_append(morph_buf_t *out, struct cli_agents_browser_state *state,
			      const struct cli_agents_browser_data *data, int columns, int rows,
			      int styled)
{
	if (!out || !state || !data || data->count < 0 || (data->count > 0 && !data->agents) ||
	    columns < 1 || rows < 1)
		return -EINVAL;
	int header = rows >= 8 ? 4 : 1;
	int footer = rows >= 3 ? 1 : 0;
	int body = rows - header - footer;
	if (body < 0)
		body = 0;
	int split = columns >= 100 && body >= 4;
	int detail_rows = !split && body >= 8 ? body / 2 : 0;
	int list_rows = body - detail_rows;
	size_t visible_count = cli_agents_browser_count(data, state->filter);
	if (detail_rows && visible_count < (size_t)list_rows) {
		list_rows = visible_count ? (int)visible_count : 1;
		detail_rows = body - list_rows;
	}
	int left_width = split ? columns * 3 / 5 : columns;
	int right_width = split ? columns - left_width - 3 : columns;
	morph_tui_widget_list_state_layout(&state->selection,
					   cli_agents_browser_count(data, state->filter),
					   (size_t)(list_rows > 0 ? list_rows : 1));
	size_t selected = cli_agents_browser_selected(state, data);
	morph_buf_t detail MORPH_CLEANUP(morph_buf_cleanup) = {0};
	morph_buf_t line MORPH_CLEANUP(morph_buf_cleanup) = {0};
	struct morph_tui_widget_text_layout layout MORPH_CLEANUP(
		morph_tui_widget_text_layout_cleanup) = {0};
	int rc = morph_buf_init(&detail, 256);
	if (rc == 0)
		rc = morph_buf_init(&line, 256);
	if (rc == 0)
		rc = cli_agents_browser_detail(&detail, data, selected, !split);
	char *safe MORPH_CLEANUP(morph_charp_cleanup) =
		rc == 0 ? utf8_terminal_sanitize_dup(detail.data, detail.len,
						     UTF8_TERMINAL_TEXT_MULTILINE, NULL)
			: NULL;
	if (rc == 0 && !safe)
		rc = -ENOMEM;
	if (rc == 0)
		rc = morph_tui_widget_text_layout_wrap_borrow(
			&layout, safe, (size_t)(right_width > 0 ? right_width : 1));
	size_t viewport = (size_t)(split ? body : detail_rows);
	size_t maximum = layout.lines.nelts > viewport ? layout.lines.nelts - viewport : 0;
	if (state->detail_scroll > maximum)
		state->detail_scroll = maximum;
	for (int row = 0; rc == 0 && row < rows; row++) {
		morph_buf_reset(&line);
		if (row < header) {
			if (!row)
				rc = morph_buf_printf(&line, "Agents (%d)  Agent command center",
						      data->count);
			else if (row == 1)
				rc = cli_agents_browser_tabs(&line, state, data, columns, styled);
			else if (row == 2) {
				for (int col = 0; rc == 0 && col < columns; col++)
					rc = morph_buf_puts(&line, "─");
			} else {
				int metadata = left_width >= 70 ? 23 : 0;
				rc = cli_agents_browser_cell(&line, "Tasks · current session",
							     left_width - metadata, ANSI_DIM,
							     styled);
				if (rc == 0 && metadata)
					rc = cli_agents_browser_cell(&line, "", 1, ANSI_DIM,
								     styled);
				if (rc == 0 && metadata)
					rc = cli_agents_browser_cell(&line, "Status", 14, ANSI_DIM,
								     styled);
				if (rc == 0 && metadata)
					rc = cli_agents_browser_cell(&line, "Elapsed", 8, ANSI_DIM,
								     styled);
			}
		} else if (footer && row == rows - 1) {
			rc = morph_buf_puts(
				&line,
				state->help ? "Esc back · Enter open · Tab/Shift+Tab filter · "
					      "Left/Right focus · PageUp/PageDown scroll"
				: state->detail_focus
					? "Esc back · ↑/↓ scroll details · Left tasks · Enter open "
					  "· ? help"
					: "Esc back · ↑/↓ move · Enter open · Tab filter · ? help");
		} else {
			int offset = row - header;
			int detail_row = split ? offset : offset - list_rows;
			if (offset < list_rows) {
				struct cli_agents_browser_state cursor = *state;
				cursor.selection.index = state->selection.first + (size_t)offset;
				size_t index = cli_agents_browser_selected(&cursor, data);
				int active = index != SIZE_MAX && index == selected;
				rc = cli_agents_browser_list_row(&line, data, index, active,
								 left_width, styled);
			}
			if (split)
				rc = rc == 0 ? morph_buf_puts(&line, " │ ") : rc;
			if (rc == 0 && detail_row >= 0) {
				size_t index =
					detail_row ? state->detail_scroll + (size_t)detail_row : 0;
				const struct morph_tui_widget_text_line *span =
					morph_array_borrow(&layout.lines, index);
				morph_buf_t text MORPH_CLEANUP(morph_buf_cleanup) = {0};
				rc = morph_buf_init(&text, 128);
				if (rc == 0 && span)
					rc = morph_buf_append(&text, safe + span->offset,
							      span->length);
				if (rc == 0)
					rc = cli_agents_browser_cell(
						&line, morph_buf_cstr_borrow(&text), right_width,
						!detail_row ? ANSI_BOLD
						: index == 3 && selected != SIZE_MAX
							? cli_agents_browser_status_style(
								  cli_agents_browser_status(
									  data, selected))
							: "",
						styled);
			}
		}
		struct cli_composer_panel_frame frame MORPH_CLEANUP(
			cli_agents_browser_frame_cleanup) = {
			.columns = columns,
			.rows = 1,
			.color = styled,
		};
		if (rc == 0)
			rc = morph_buf_init(&frame.content, 128);
		if (rc == 0)
			rc = cli_composer_panel_styled_line(&frame, morph_buf_cstr_borrow(&line));
		if (rc == 0 && styled)
			rc = morph_buf_append(out, frame.content.data, frame.content.len);
		else if (rc == 0) {
			char *plain MORPH_CLEANUP(morph_charp_cleanup) =
				utf8_terminal_sanitize_dup(frame.content.data, frame.content.len,
							   UTF8_TERMINAL_TEXT_MULTILINE, NULL);
			rc = plain ? morph_buf_puts(out, plain) : -ENOMEM;
		}
	}
	return rc;
}
