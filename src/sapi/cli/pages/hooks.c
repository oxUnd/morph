#include "sapi/cli/pages/hooks.h"
#include "sapi/cli/components/page_layout.h"
#include "sapi/cli/presentation/panel.h"
#include <nothing/tui/component_state.h>
#include "cJSON.h"
#include <nothing/tui/models/list_navigation.h>
#include <nothing/tui/controls/list_view.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/text_view.h>
#include <nothing/tui/views/scroll_view.h>
#include <nothing/tui/terminal/text.h>
#include <nothing/tui/view_controller.h>
#include <nothing/tui/terminal.h>
#include "sapi/cli/application.h"
#include "sapi/cli/pages/setup/controls.h"
#include "runtime/runtime.h"
#include <nb/cleanup.h>
#include <nb/utf8.h>
#include <nb/vec.h>
#include <nb/buf.h>
#include "errors.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum cli_hooks_style {
	HOOK_TEXT,
	HOOK_BOLD,
	HOOK_DIM,
	HOOK_CYAN,
	HOOK_GREEN,
	HOOK_AMBER,
	HOOK_RED,
	HOOK_ADD,
	HOOK_SELECTED
};

struct cli_hooks_line {
	char *text;
	enum cli_hooks_style style;
};

enum cli_hooks_section { HOOKS_HANDLERS, HOOKS_EVENTS, HOOKS_REVIEW };

struct cli_hooks_navigation {
	enum cli_hooks_section section;
	int event_filter;
	int batch;
};

enum cli_hooks_transition {
	SHOW_EVENTS,
	FILTER_EVENT,
	REVIEW_HOOK,
	SHOW_HANDLERS,
	REVIEW_BATCH,
	CLOSE_BATCH,
	GO_BACK,
	SHOW_ALL_HOOKS
};

struct cli_hooks_action_input {
	enum cli_hooks_transition transition;
	int event;
};

struct cli_hooks_page {
	struct nt_view_controller base;
	struct cli_page_layout layout;
	cJSON *rows;
	struct nt_view *root, *title, *subtitle, *scope, *rule, *heading, *list,
		*range, *divider, *viewport, *document, *choice_title, *choices, *message_view,
		*help;
	struct cli_hooks_navigation initial;
	struct nt_component_state *navigation;
	int height, columns, width, focus, event_focus, scroll, choice, page_size, document_height,
		document_lines, first, syncing;
	char message[256];
};

static const struct cli_hooks_navigation *cli_hooks_navigation(const struct cli_hooks_page *s)
{
	return nt_component_state_get(s->navigation);
}

static int cli_hooks_reduce(const void *previous, const void *argument, size_t size, void *next,
			    void *data)
{
	struct cli_hooks_action_input action;
	(void)data;
	if (size != sizeof(action))
		return NB_ERR_INVAL;

	memcpy(&action, argument, sizeof(action));
	struct cli_hooks_navigation nav;
	memcpy(&nav, previous, sizeof(nav));
	switch (action.transition) {
	case SHOW_ALL_HOOKS:
		nav = (struct cli_hooks_navigation){.section = HOOKS_HANDLERS};
		break;
	case SHOW_EVENTS:
		nav.section = HOOKS_EVENTS;
		break;
	case FILTER_EVENT:
		nav.event_filter = action.event + 1;
		nav.section = HOOKS_HANDLERS;
		break;
	case REVIEW_HOOK:
		nav.section = HOOKS_REVIEW;
		break;
	case SHOW_HANDLERS:
		nav.section = HOOKS_HANDLERS;
		break;
	case REVIEW_BATCH:
	case CLOSE_BATCH:
		nav.batch = action.transition == REVIEW_BATCH;
		break;
	case GO_BACK:
		if (nav.section == HOOKS_EVENTS)
			nav.event_filter = 0;

		nav.section = nav.section == HOOKS_HANDLERS && nav.event_filter ? HOOKS_EVENTS
										: HOOKS_HANDLERS;
		break;
	}

	memcpy(next, &nav, sizeof(nav));
	return 0;
}

static int cli_hooks_navigate(struct cli_hooks_page *s, enum cli_hooks_transition transition)
{
	struct cli_hooks_action_input action = {transition, s->event_focus};
	int rc = nt_component_state_dispatch(s->navigation, &action, sizeof(action));
	if (!rc)
		rc = nt_view_flush_state(s->root);

	return rc ? rc : NT_NAVIGATION_CHANGED;
}

static struct nt_text_style cli_hooks_style(enum cli_hooks_style id)
{
	struct nt_text_style style = {0};
	static const unsigned colors[] = {[HOOK_CYAN] = 0x47d3e6,
					  [HOOK_GREEN] = 0x7cd9a7,
					  [HOOK_AMBER] = 0xe2b95b,
					  [HOOK_RED] = 0xf0a3a3,
					  [HOOK_ADD] = 0xa0e3b6};
	if (id == HOOK_BOLD || id == HOOK_SELECTED)
		style = cli_panel_heading(NT_AUTO).style;

	if (id == HOOK_DIM)
		style = cli_panel_hint(NT_AUTO).style;

	if (id >= HOOK_CYAN && id <= HOOK_ADD)
		style.foreground = (struct nt_color){NT_COLOR_RGB, colors[id]};

	if (id == HOOK_SELECTED)
		style.foreground = (struct nt_color){NT_COLOR_ANSI, 6};

	return style;
}

struct cli_hooks_action {
	const char *name;
	const char *label;
};

static const char *cli_hooks_field(const cJSON *row, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(row, key);

	return cJSON_IsString(value) ? value->valuestring : "";
}

static const char *cli_hooks_state_label(const cJSON *row)
{
	const char *state = cli_hooks_field(row, "state");

	if (strcmp(state, "trusted") == 0)
		return "Trusted";

	if (strcmp(state, "needs_review") == 0)
		return "Needs review";

	if (strcmp(state, "changed / needs_review") == 0)
		return "Changed / needs review";

	if (strcmp(state, "project_untrusted") == 0)
		return "Workspace untrusted";

	if (strcmp(state, "disabled") == 0)
		return "Disabled";

	if (strcmp(state, "off") == 0)
		return "Hooks off";

	if (strcmp(state, "invalid") == 0)
		return "Invalid";

	if (strcmp(state, "unsupported") == 0)
		return "Unsupported";

	return state;
}

static enum cli_hooks_style cli_hooks_state_style(const cJSON *row)
{
	const char *state = cli_hooks_field(row, "state");

	if (strcmp(state, "trusted") == 0)
		return HOOK_GREEN;

	if (strcmp(state, "disabled") == 0 || strcmp(state, "off") == 0)
		return HOOK_DIM;

	return HOOK_AMBER;
}

static int cli_hooks_actions(const cJSON *row, struct cli_hooks_action items[4])
{
	const char *state = cli_hooks_field(row, "state");
	int count = 0;

	if (strcmp(state, "trusted") == 0) {
		items[count++] = (struct cli_hooks_action){"revoke", "Revoke trust"};
		items[count++] = (struct cli_hooks_action){"disable", "Disable hook"};
	} else if (strcmp(state, "disabled") == 0) {
		items[count++] = (struct cli_hooks_action){"enable", "Enable hook"};
		items[count++] = (struct cli_hooks_action){"revoke", "Revoke trust"};
	} else if (strcmp(state, "project_untrusted") == 0) {
		items[count++] = (struct cli_hooks_action){"trust-project", "Trust this workspace"};
		items[count++] = (struct cli_hooks_action){"disable", "Disable hook"};
	} else if (strcmp(state, "needs_review") == 0 ||
		   strcmp(state, "changed / needs_review") == 0) {
		items[count++] = (struct cli_hooks_action){"trust", "Trust current definition"};
		items[count++] = (struct cli_hooks_action){"disable", "Disable hook"};
	}

	items[count++] = (struct cli_hooks_action){
		NULL,
		strcmp(state, "trusted") == 0 ? "Back to hooks" : "Keep unchanged / Back to hooks"};
	return count;
}

#define CLI_HOOKS_HOOK_EVENT_COUNT 12

static const char *cli_hooks_event_name(int index)
{
	const char *names[] = {
		"PreToolUse",	 "PermissionRequest", "PostToolUse", "PreCompact",
		"PostCompact",	 "SessionStart",      "SessionEnd",  "UserPromptSubmit",
		"SubagentStart", "SubagentStop",      "Stop",	     "Interrupt"};

	return index >= 0 && index < CLI_HOOKS_HOOK_EVENT_COUNT ? names[index] : "";
}

static int cli_hooks_view_count(const struct cli_hooks_page *screen, const cJSON *rows)
{
	const cJSON *row;
	int count = 0;

	int filter = cli_hooks_navigation(screen)->event_filter;
	if (!filter)
		return cJSON_GetArraySize(rows);

	cJSON_ArrayForEach (row, rows) {
		if (strcmp(cli_hooks_field(row, "event"), cli_hooks_event_name(filter - 1)) == 0)
			count++;
	}

	return count;
}

static const cJSON *cli_hooks_view_row(const struct cli_hooks_page *screen, const cJSON *rows,
				       int index)
{
	const cJSON *row;

	int filter = cli_hooks_navigation(screen)->event_filter;
	if (!filter)
		return cJSON_GetArrayItem(rows, index);

	cJSON_ArrayForEach (row, rows) {
		if (strcmp(cli_hooks_field(row, "event"), cli_hooks_event_name(filter - 1)) == 0 &&
		    index-- == 0)
			return row;
	}

	return NULL;
}

static int cli_hooks_add_line(nb_vec *lines, const char *text, enum cli_hooks_style style)
{
	char *safe NB_CLEANUP(nb_charp_cleanup) =
		nt_sanitize_dup(text, strlen(text), NT_TEXT_SINGLE_LINE, NULL);
	struct cli_hooks_line *line = safe ? nb_vec_push(lines) : NULL;
	if (!line)
		return NB_ERR_NOMEM;

	*line = (struct cli_hooks_line){nb_charp_steal(&safe), style};
	return 0;
}

static int cli_hooks_detail_field(nb_vec *lines, nb_buf *buffer, const char *label,
				  const char *value)
{
	nb_buf_clear(buffer);
	int rc =
		(nb_buf_printf(buffer, "%-13s %s", label, *value ? value : "—") ? 0 : NB_ERR_NOMEM);

	return rc == 0 ? cli_hooks_add_line(lines, nb_buf_cstr(buffer), HOOK_TEXT) : rc;
}

static int cli_hooks_diff_property(nb_vec *lines, nb_buf *buffer, const cJSON *value,
				   const char *prefix, enum cli_hooks_style style)
{
	char *json NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(value);
	if (!json)
		return NB_ERR_NOMEM;

	nb_buf_clear(buffer);
	int rc = (nb_buf_printf(buffer, "%s  \"%s\": %s%s", prefix, value->string, json,
				value->next ? "," : "")
			  ? 0
			  : NB_ERR_NOMEM);
	return rc == 0 ? cli_hooks_add_line(lines, nb_buf_cstr(buffer), style) : rc;
}

static int cli_hooks_definition_lines(nb_vec *lines, nb_buf *buffer, const cJSON *row)
{
	cJSON *current NB_CLEANUP(cJSON_DeleteOwner) =
		cJSON_Parse(cli_hooks_field(row, "definition"));
	cJSON *previous NB_CLEANUP(cJSON_DeleteOwner) =
		cJSON_Parse(cli_hooks_field(row, "previous_definition"));
	const cJSON *value;
	if (previous && cJSON_Compare(current, previous, 1)) {
		cJSON_DeleteOwner(&previous);
	}

	if (!cJSON_IsObject(current)) {
		return NB_ERR_PARSE;
	}

	int rc = cli_hooks_add_line(lines, previous ? "Definition changes" : "Current definition",
				    HOOK_BOLD);

	if (rc == 0)
		rc = cli_hooks_add_line(lines, "{", HOOK_DIM);

	cJSON_ArrayForEach (value, previous) {
		if (rc != 0)
			break;

		if (!cJSON_HasObjectItem(current, value->string))
			rc = cli_hooks_diff_property(lines, buffer, value, "-", HOOK_RED);
	}

	cJSON_ArrayForEach (value, current) {
		if (rc != 0)
			break;

		const cJSON *old = cJSON_GetObjectItemCaseSensitive(previous, value->string);
		int changed = previous && !cJSON_Compare(old, value, 1);

		if (changed && old)
			rc = cli_hooks_diff_property(lines, buffer, old, "-", HOOK_RED);

		if (rc == 0)
			rc = cli_hooks_diff_property(lines, buffer, value, changed ? "+" : " ",
						     changed ? HOOK_ADD : HOOK_TEXT);
	}

	if (rc == 0)
		rc = cli_hooks_add_line(lines, "}", HOOK_DIM);

	return rc;
}

static int cli_hooks_document(nb_vec *lines, const cJSON *row, int review)
{
	nb_buf buffer NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&buffer), nb_buf_reserve(&buffer, 256) ? 0 : NB_ERR_NOMEM);
	const char *keys[] = {"id", "command", "matcher", "source", "cwd"};
	const char *labels[] = {"ID", "Command", "Matcher", "Source", "Working dir"};

	if (rc == 0 && !review) {
		rc = (nb_buf_printf(&buffer, "%s · %s", cli_hooks_field(row, "event"),
				    cli_hooks_state_label(row))
			      ? 0
			      : NB_ERR_NOMEM);
		if (rc == 0)
			rc = cli_hooks_add_line(lines, nb_buf_cstr(&buffer), HOOK_BOLD);
	}

	for (size_t i = 0; rc == 0 && i < sizeof(keys) / sizeof(keys[0]); i++)
		rc = cli_hooks_detail_field(lines, &buffer, labels[i],
					    cli_hooks_field(row, keys[i]));

	if (rc == 0 && *cli_hooks_field(row, "diagnostic"))
		rc = cli_hooks_detail_field(lines, &buffer, "Unavailable",
					    cli_hooks_field(row, "diagnostic"));

	if (rc == 0 && *cli_hooks_field(row, "statusMessage"))
		rc = cli_hooks_detail_field(lines, &buffer, "While running",
					    cli_hooks_field(row, "statusMessage"));

	if (rc == 0) {
		const cJSON *timeout = cJSON_GetObjectItemCaseSensitive(row, "timeout");
		char number[32];

		snprintf(number, sizeof(number), "%ds",
			 cJSON_IsNumber(timeout) ? timeout->valueint : 0);
		rc = cli_hooks_detail_field(lines, &buffer, "Timeout", number);
	}

	if (rc == 0 && review)
		rc = cli_hooks_add_line(lines, "", HOOK_DIM);

	if (rc == 0 && review)
		rc = cli_hooks_definition_lines(lines, &buffer, row);

	if (rc == 0)
		rc = cli_hooks_add_line(lines, "", HOOK_DIM);

	if (rc == 0)
		rc = cli_hooks_add_line(lines, "Last execution", HOOK_BOLD);

	const cJSON *last = cJSON_GetObjectItemCaseSensitive(row, "last_run");
	if (rc == 0 && last) {
		nb_buf_clear(&buffer);
		rc = (nb_buf_append_cstr(&buffer, cli_hooks_field(last, "name")) ? 0
										 : NB_ERR_NOMEM);
		const cJSON *exit = cJSON_GetObjectItemCaseSensitive(last, "exit_code");
		const cJSON *duration = cJSON_GetObjectItemCaseSensitive(last, "duration_ms");

		if (rc == 0 && cJSON_IsNumber(exit))
			rc = (nb_buf_printf(&buffer, " · exit %d", exit->valueint) ? 0
										   : NB_ERR_NOMEM);

		if (rc == 0 && cJSON_IsNumber(duration))
			rc = (nb_buf_printf(&buffer, " · %.0f ms", duration->valuedouble)
				      ? 0
				      : NB_ERR_NOMEM);

		if (rc == 0)
			rc = cli_hooks_add_line(lines, nb_buf_cstr(&buffer), HOOK_DIM);

		if (rc == 0 && *cli_hooks_field(last, "error"))
			rc = cli_hooks_detail_field(lines, &buffer, "Error",
						    cli_hooks_field(last, "error"));

		if (rc == 0 && *cli_hooks_field(last, "stderr"))
			rc = cli_hooks_detail_field(lines, &buffer, "Stderr",
						    cli_hooks_field(last, "stderr"));
	} else if (rc == 0) {
		rc = cli_hooks_add_line(lines, "Not run", HOOK_DIM);
	}

	if (rc == 0 && review)
		rc = cli_hooks_add_line(
			lines, "Trust covers this definition, not mutable script contents.",
			HOOK_DIM);

	if (rc == 0 && strcmp(cli_hooks_field(row, "state"), "project_untrusted") == 0)
		rc = cli_hooks_add_line(lines,
					"Review this workspace path before trusting it; then "
					"review the hook definition.",
					HOOK_AMBER);

	return rc;
}

static void cli_hooks_lines_cleanup(nb_vec *lines)
{
	for (size_t i = 0; i < lines->len; i++) {
		struct cli_hooks_line *line = nb_vec_at(lines, i);
		free(line->text);
	}

	nb_vec_cleanup(lines);
}

static int cli_hooks_place(struct nt_view *view, int x, int y, int width, int height)
{
	int rc = nt_view_set_hidden(view, height < 1 || width < 1);
	if (rc || height < 1 || width < 1)
		return rc;

	return nt_view_set_frame(view, (struct nt_view_rect){x, y, width, height});
}

static char *cli_hooks_clipped(const char *text, int width)
{
	char *clean NB_CLEANUP(nb_charp_cleanup) =
		nt_sanitize_dup(text, strlen(text), NT_TEXT_SINGLE_LINE, NULL);
	if (!clean)
		return NULL;

	size_t columns = width > 0 ? (size_t)width : 0;
	if (nb_utf8_width_cstr(clean) <= columns)
		return nb_charp_steal(&clean);

	size_t capacity = strlen(clean) + 4;
	char *clipped = malloc(capacity);
	if (clipped)
		nt_terminal_copy_ellipsized_display_width(clipped, capacity, clean, columns, 0);

	return clipped;
}

static int cli_hooks_label(struct nt_view *view, const char *text, int width,
			   enum cli_hooks_style style)
{
	char *clipped NB_CLEANUP(nb_charp_cleanup) = cli_hooks_clipped(text, width);
	if (!clipped)
		return NB_ERR_NOMEM;

	int rc = nt_view_set_style(view, cli_hooks_style(style));
	return rc ? rc : nt_label_set_text(view, clipped);
}

static int cli_hooks_is_pending(const cJSON *row)
{
	const char *state = cli_hooks_field(row, "state");
	return !strcmp(state, "needs_review") || !strcmp(state, "changed / needs_review") ||
	       !strcmp(state, "project_untrusted");
}

static int cli_hooks_cell(struct nt_canvas *row, int x, int width, const char *text,
			  struct nt_text_style style)
{
	if (width <= 0)
		return 0;

	struct nt_canvas column;
	nt_canvas_subcanvas(row, x, 0, width, 1, &column);
	char *clipped NB_CLEANUP(nb_charp_cleanup) = cli_hooks_clipped(text, width);
	if (!clipped)
		return NB_ERR_NOMEM;

	int rc = nt_canvas_fill_styled(&column, &style);
	return rc ? rc : nt_canvas_text_styled(&column, 0, 0, clipped, &style);
}

static const char *cli_hooks_source(void *data, size_t index)
{
	struct cli_hooks_page *s = data;
	int i = s->first + (int)index;
	return cli_hooks_navigation(s)->section == HOOKS_EVENTS
		       ? cli_hooks_event_name(i)
		       : cli_hooks_field(cli_hooks_view_row(s, s->rows, i), "event");
}

struct cli_hooks_columns {
	int event, command, source, state;
};

static struct cli_hooks_columns cli_hooks_columns(int w)
{
	struct cli_hooks_columns c = {.event = w >= 68	 ? 22
					       : w >= 40 ? 20
							 : w,
				      .source = w >= 90 ? 10 : 0,
				      .state = w >= 68	 ? 22
					       : w >= 40 ? 16
							 : 0};
	c.command = w - c.event - c.source - c.state;
	return c;
}

static int cli_hooks_row(void *data, struct nt_canvas *line, size_t index, size_t group,
			 int selected)
{
	struct cli_hooks_page *s = data;
	int i = (int)index + s->first;
	struct nt_text_style style = cli_hooks_style(selected ? HOOK_SELECTED : HOOK_TEXT);
	(void)group;
	int rc = selected ? nt_canvas_fill_styled(line, &style) : 0;
	if (!rc && selected)
		rc = cli_hooks_cell(line, 0, 1, "▏", style);

	if (rc)
		return rc;

	if (cli_hooks_navigation(s)->section == HOOKS_EVENTS) {
		const cJSON *row;
		int installed = 0, active = 0, pending = 0;
		const char *name = cli_hooks_event_name(i);
		cJSON_ArrayForEach (row, s->rows) {
			if (!strcmp(cli_hooks_field(row, "event"), name)) {
				installed++;
				active += !strcmp(cli_hooks_field(row, "state"), "trusted");
				pending += cli_hooks_is_pending(row);
			}
		}

		char text[128];
		snprintf(text, sizeof(text), "%-22s %-10d %-7d %d", name, installed, active,
			 pending);
		return cli_hooks_cell(line, 1, s->width, text, style);
	}

	const cJSON *row = cli_hooks_view_row(s, s->rows, i);
	struct cli_hooks_columns c = cli_hooks_columns(s->width);
	rc = cli_hooks_cell(line, 1, c.event, cli_hooks_field(row, "event"), style);
	if (!rc && c.command > 6)
		rc = cli_hooks_cell(line, 1 + c.event, c.command - 2,
				    cli_hooks_field(row, "command"), style);

	int source = 1 + c.event + c.command;
	if (!rc)
		rc = cli_hooks_cell(line, source, c.source,
				    cJSON_IsTrue(cJSON_GetObjectItem(row, "project")) ? "Project"
										      : "User",
				    style);

	struct nt_text_style state = cli_hooks_style(cli_hooks_state_style(row));
	if (selected)
		state.attributes |= NT_STYLE_BOLD;

	return rc ? rc
		  : cli_hooks_cell(line, source + c.source, c.state, cli_hooks_state_label(row),
				   state);
}

static const char *cli_hooks_choice(void *data, size_t index)
{
	struct cli_hooks_page *s = data;
	static const char *const batch[] = {"Trust all current hooks and remember",
					    "Reject all current hooks and remember",
					    "Review individually / keep unchanged"};
	if (cli_hooks_navigation(s)->batch)
		return index < 3 ? batch[index] : "";

	struct cli_hooks_action items[4];
	int count = cli_hooks_actions(cli_hooks_view_row(s, s->rows, s->focus), items);
	return index < (size_t)count ? items[index].label : "";
}

static int cli_hooks_choice_row(void *data, struct nt_canvas *line, size_t index, size_t group,
				int selected)
{
	struct nt_text_style style = cli_hooks_style(selected ? HOOK_SELECTED : HOOK_TEXT);
	(void)group;
	int rc = selected ? cli_hooks_cell(line, 0, 1, "▏", style) : 0;
	return rc ? rc
		  : cli_hooks_cell(line, 1, line->width - 1, cli_hooks_choice(data, index), style);
}

static int cli_hooks_rules(struct cli_hooks_page *s)
{
	nb_buf rule NB_CLEANUP(nb_buf_cleanup) = {0};
	for (int i = 0; i < s->width; i++)
		nb_buf_append_cstr(&rule, "─");

	int rc = nb_buf_status(&rule);
	if (!rc)
		rc = nt_label_set_text(s->rule, nb_buf_cstr(&rule));

	return rc ? rc : nt_label_set_text(s->divider, nb_buf_cstr(&rule));
}

static int cli_hooks_text(struct cli_hooks_page *s, nb_vec *lines, int wrap)
{
	nb_vec spans NB_CLEANUP(nb_vec_cleanup) = {0};
	struct nt_text_span span = {0};
	nb_vec_init(&spans, sizeof(span));
	for (size_t i = 0; i < lines->len; i++) {
		struct cli_hooks_line *line = nb_vec_at(lines, i);
		if (!wrap) {
			char *clipped = cli_hooks_clipped(line->text, s->width);
			if (!clipped)
				return NB_ERR_NOMEM;

			free(line->text);
			line->text = clipped;
		}

		if (i) {
			span = (struct nt_text_span){.text = "\n"};
			if (!nb_vec_push_copy(&spans, &span))
				return NB_ERR_NOMEM;
		}

		span = (struct nt_text_span){.text = line->text,
					     .style = cli_hooks_style(line->style)};
		if (!nb_vec_push_copy(&spans, &span))
			return NB_ERR_NOMEM;
	}

	int rc = nt_text_view_set_wrap_mode(s->document,
					    wrap ? NT_TEXT_WRAP_CHAR : NT_TEXT_WRAP_NONE);
	if (!rc)
		rc = nt_text_view_set_spans(s->document, nb_vec_at(&spans, 0), spans.len);

	struct nt_view_size size;
	if (!rc)
		rc = nt_view_measure(s->document, s->width, &size);

	if (!rc)
		s->document_lines = size.height;

	return rc;
}

static int cli_hooks_batch_text(struct cli_hooks_page *s, nb_vec *lines)
{
	char label[256];
	for (int i = 0; i < s->page_size; i++) {
		const cJSON *row = cJSON_GetArrayItem(s->rows, s->scroll + i);
		if (!row)
			break;

		snprintf(label, sizeof(label), "%s · %s", cli_hooks_field(row, "event"),
			 cli_hooks_state_label(row));
		int rc = cli_hooks_add_line(lines, label, cli_hooks_state_style(row));
		const char *const keys[] = {"source", "command"};
		for (size_t j = 0; !rc && j < 2; j++) {
			nb_buf value NB_CLEANUP(nb_buf_cleanup) = {0};
			rc = nb_buf_printf(&value, "  %s", cli_hooks_field(row, keys[j]))
				     ? 0
				     : NB_ERR_NOMEM;
			if (!rc)
				rc = cli_hooks_add_line(lines, nb_buf_cstr(&value),
							j ? HOOK_TEXT : HOOK_DIM);
		}

		if (rc)
			return rc;
	}

	return 0;
}

static int cli_hooks_sync(struct cli_hooks_page *s)
{
	s->syncing = 1;
	const struct cli_hooks_navigation *nav = cli_hooks_navigation(s);
	int h = s->height, w = s->width;
	int small = h < (nav->batch ? 18 : 10) || w < (nav->batch ? 40 : 24);
	int count = cli_hooks_view_count(s, s->rows);
	int batch = !small && nav->batch, events = !small && !batch && nav->section == HOOKS_EVENTS;
	int empty = !small && !batch && !events && !count;
	int review = !small && !batch && !empty && !events && nav->section == HOOKS_REVIEW;
	int list = !small && !batch && !empty && !events && !review;
	const cJSON *row = cli_hooks_view_row(s, s->rows, s->focus);
	struct cli_hooks_action actions[4];
	int choices = batch ? 3 : review ? cli_hooks_actions(row, actions) : 0;
	int page = batch ? (h - 12) / 3 : list && h >= 20 ? (h - 16) / 2 : h - 6;
	s->page_size = page > 0 ? page : 1;
	int total = events ? CLI_HOOKS_HOOK_EVENT_COUNT : count;
	int selected = events ? s->event_focus : s->focus;
	s->first = selected / s->page_size * s->page_size;
	int visible = total - s->first;
	if (visible > s->page_size)
		visible = s->page_size;

	int document_y = batch ? 4 : empty ? 3 : review ? 3 : h >= 20 ? 6 + visible : h - 2;
	int bottom = batch ? h - 7 : review ? h - choices - 3 : h - 1;
	s->document_height =
		(batch || review || list || empty) && bottom > document_y ? bottom - document_y : 0;
	char title[160] = "", subtitle[200] = "", heading[200] = "", range[128] = "",
	     status[256] = "";
	const char *hint = "Esc Back / q Close";
	enum cli_hooks_style subtitle_style = HOOK_DIM;
	if (small) {
		snprintf(title, sizeof(title), "Resize terminal to view hooks.");
	} else if (batch) {
		snprintf(title, sizeof(title), "Trust lifecycle hooks?");
		snprintf(subtitle, sizeof(subtitle), "%s",
			 cli_hooks_field(cJSON_GetArrayItem(s->rows, 0), "cwd"));
		s->scroll = (int)nt_scroll_offset_after_key((size_t)s->scroll, (size_t)count,
							    (size_t)s->page_size, 0);
		int end = s->scroll + s->page_size;
		snprintf(range, sizeof(range), "Showing %d–%d of %d · PgUp/PgDn to scroll",
			 count ? s->scroll + 1 : 0, end < count ? end : count, count);
		hint = "↑↓ Choose  Enter Confirm  Esc Keep unchanged";
	} else if (events) {
		snprintf(title, sizeof(title), "Lifecycle events");
		snprintf(subtitle, sizeof(subtitle),
			 "Inspect installed, active, and pending hooks by event.");
		snprintf(heading, sizeof(heading),
			 "EVENT                  INSTALLED  ACTIVE  REVIEW");
		hint = "↑↓ Select  Enter Handlers  r Refresh  Esc All hooks";
	} else if (review) {
		snprintf(title, sizeof(title), "%s", cli_hooks_field(row, "event"));
		snprintf(subtitle, sizeof(subtitle), "%s", cli_hooks_state_label(row));
		subtitle_style = cli_hooks_state_style(row);
		hint = "↑↓ Choose  Enter Confirm  PgUp/PgDn Scroll  Esc Back";
	} else if (empty) {
		snprintf(title, sizeof(title), "%s",
			 nav->event_filter ? cli_hooks_event_name(nav->event_filter - 1)
					   : "Lifecycle hooks");
		hint = "e Events  r Refresh  Esc Back";
	} else {
		const cJSON *item;
		int trusted = 0, pending = 0, disabled = 0;
		cJSON_ArrayForEach (item, s->rows) {
			trusted += !strcmp(cli_hooks_field(item, "state"), "trusted");
			disabled += !strcmp(cli_hooks_field(item, "state"), "disabled");
			pending += cli_hooks_is_pending(item);
		}

		snprintf(title, sizeof(title), "Lifecycle hooks%s%s",
			 nav->event_filter ? " · " : "",
			 nav->event_filter ? cli_hooks_event_name(nav->event_filter - 1) : "");
		int used = snprintf(subtitle, sizeof(subtitle),
				    "%d trusted · %d need review · %d disabled", trusted, pending,
				    disabled);
		if (count > trusted + pending + disabled)
			snprintf(subtitle + used, sizeof(subtitle) - (size_t)used,
				 " · %d unavailable", count - trusted - pending - disabled);

		struct cli_hooks_columns c = cli_hooks_columns(w);
		snprintf(heading, sizeof(heading), "%-*s%-*s%-*s%s", c.event, "EVENT", c.command,
			 c.command > 6 ? "COMMAND" : "", c.source, c.source ? "SOURCE" : "",
			 c.state ? "STATE" : "");
		snprintf(range, sizeof(range), "Showing %d–%d of %d", s->first + 1,
			 s->first + visible, total);
		hint = count > s->page_size ? "↑↓ Select  Enter Review  e Events  a Review all  ←→ "
					      "Page  r Refresh  Esc Close"
					    : "↑↓ Select  Enter Review  e Events  a Review all  r "
					      "Refresh  Esc Close";
	}

	nb_vec lines NB_CLEANUP(cli_hooks_lines_cleanup) = {0};
	struct cli_hooks_line line;
	nb_vec_init(&lines, sizeof(line));
	int rc = 0;
	if (batch)
		rc = cli_hooks_batch_text(s, &lines);
	else if (empty) {
		rc = cli_hooks_add_line(&lines,
					nav->event_filter ? "No hooks configured for this event."
							  : "No hooks configured.",
					HOOK_TEXT);
		const char *const setup[] = {
			"", "Add hook definitions to:", "~/.morph/hooks.json   (user)",
			".morph/hooks.json     (project)"};
		for (size_t i = 0; !rc && h >= 14 && i < 4; i++)
			rc = cli_hooks_add_line(&lines, setup[i], HOOK_DIM);
	} else if ((list || review) && row)
		rc = cli_hooks_document(&lines, row, review);

	if (!rc)
		rc = cli_hooks_text(s, &lines, list || review);

	int offset = review ? (int)nt_scroll_offset_after_key((size_t)s->scroll,
							      (size_t)s->document_lines,
							      (size_t)s->document_height, 0)
			    : 0;
	if (review)
		s->scroll = offset;

	if (!rc)
		rc = nt_scroll_view_set_offset(s->viewport, offset);

	if (!rc)
		rc = cli_hooks_rules(s);

	if (!rc)
		rc = cli_hooks_label(s->title, title, w, small ? HOOK_AMBER : HOOK_BOLD);

	if (!rc)
		rc = cli_hooks_label(s->subtitle, subtitle, w, subtitle_style);

	if (!rc)
		rc = cli_hooks_label(s->heading, heading, w, HOOK_DIM);

	if (!rc)
		rc = cli_hooks_label(s->range, range, w, HOOK_DIM);

	if (!rc)
		rc = cli_page_layout_set_subpage(&s->layout,
			nav->batch ? "review all" : nav->section == HOOKS_REVIEW ? "review"
			: nav->section == HOOKS_EVENTS ? "events"
			: nav->event_filter ? "handlers" : "");

	if (!rc)
		rc = cli_hooks_label(s->help, hint, w, HOOK_DIM);

	if (!rc && batch)
		rc = cli_hooks_label(
			s->scope, "User hooks: all workspaces. Project hooks: this workspace only.",
			w, HOOK_DIM);

	if (*s->message)
		snprintf(status, sizeof(status), "%s", s->message);
	else if (review && s->document_lines > s->document_height)
		snprintf(status, sizeof(status), "Lines %d–%d / %d · PgUp/PgDn to read more",
			 offset + 1, offset + s->document_height, s->document_lines);

	if (!rc)
		rc = cli_hooks_label(s->message_view, status, w,
				     batch || *s->message ? HOOK_AMBER : HOOK_DIM);

	struct {
		struct nt_view *view;
		int x, y, width, height;
	} parts[] = {{s->title, 0, 0, w, 1},
		     {s->subtitle, 0, 1, w, *subtitle != 0},
		     {s->scope, 0, 2, w, batch},
		     {s->rule, 0, batch ? 3 : 2, w, !small && !empty},
		     {s->heading, 0, 3, w, list || events},
		     {s->list, 0, 4, w, list || events ? visible : 0},
		     {s->range, 0, batch ? h - 7 : 4 + visible, w, batch || list},
		     {s->divider, 0,
		      batch    ? h - 6
		      : review ? h - choices - 3
			       : document_y - 1,
		      w, batch || review || (list && h >= 20)},
		     {s->viewport, 0, document_y, w, s->document_height},
		     {s->choice_title, 0, h - choices - 2, w, review},
		     {s->choices, 0, batch ? h - 5 : h - choices - 1, w, choices},
		     {s->message_view, 0, h - 1, w, batch || status[0] != 0}};
	for (size_t i = 0; !rc && i < sizeof(parts) / sizeof(parts[0]); i++)
		rc = cli_hooks_place(parts[i].view, parts[i].x, parts[i].y, parts[i].width,
				     parts[i].height);

	if (!rc && (list || events))
		rc = nt_list_view_set_source(s->list, (size_t)visible, cli_hooks_source, s);

	if (!rc && (list || events) && visible)
		rc = nt_list_view_select(s->list, (size_t)(selected - s->first));

	if (!rc && choices)
		rc = nt_list_view_set_source(s->choices, (size_t)choices, cli_hooks_choice, s);

	if (!rc && choices)
		rc = nt_list_view_select(s->choices, (size_t)s->choice);

	s->syncing = 0;
	return rc;
}

static int cli_hooks_refresh(struct cli_application *ctx, cJSON **rows,
			     struct cli_hooks_page *screen)
{
	cJSON *fresh = NULL;
	const cJSON *row = cli_hooks_view_row(screen, *rows, screen->focus);
	char id[65];

	snprintf(id, sizeof(id), "%s", cli_hooks_field(row, "id"));
	int rc = runtime_hooks_snapshot_copy(ctx->runtime, &fresh);
	if (rc != 0)
		return rc;

	cJSON_Delete(*rows);
	*rows = fresh;
	screen->focus = 0;
	for (int i = 0; i < cli_hooks_view_count(screen, fresh); i++) {
		row = cli_hooks_view_row(screen, fresh, i);
		if (strcmp(cli_hooks_field(row, "id"), id) == 0)
			break;

		screen->focus++;
	}

	if (screen->focus >= cli_hooks_view_count(screen, fresh)) {
		screen->focus = 0;
		if (cli_hooks_navigation(screen)->section == HOOKS_REVIEW) {
			rc = cli_hooks_navigate(screen, SHOW_HANDLERS);
			if (rc < 0)
				return rc;
		}
	}

	struct cli_hooks_action items[4];
	screen->choice =
		cli_hooks_actions(cli_hooks_view_row(screen, fresh, screen->focus), items) - 1;
	screen->scroll = 0;
	return 0;
}

static void cli_hooks_choose(int *index, int count, int page, int key, unsigned flags)
{
	struct nt_menu_state menu = {0};

	menu.list.index = (size_t)*index;
	menu.list.flags = flags;
	if ((flags & NT_LIST_VIEW_WRAP) && key != NT_KEY_UP && key != NT_KEY_DOWN && key != 'k' &&
	    key != 'j' && key != '\t')
		key = 0;

	nt_list_state_layout(&menu.list, (size_t)count, page > 0 ? (size_t)page : 1);
	(void)nt_menu_state_update(&menu, key);
	*index = (int)menu.list.index;
}

static int cli_hooks_update(void *data, const struct nt_event *event)
{
	struct cli_hooks_page *screen = data;
	struct cli_application *ctx = cli_application_from_controller(&screen->base);
	cJSON **rows = &screen->rows;

	if (event->type != NT_EVENT_KEY || event->pasting)
		return NT_NAVIGATION_UNHANDLED;

	struct cli_hooks_navigation nav = *cli_hooks_navigation(screen);
	int key = event->key;
	int rc;

	if (key == 3 || key == 4 || key == 'q')
		return NT_NAVIGATION_BACK;

	if (key == 27) {
		if (nav.batch)
			return cli_hooks_navigate(screen, CLOSE_BATCH);

		if (nav.section == HOOKS_HANDLERS && !nav.event_filter)
			return NT_NAVIGATION_BACK;

		if (nav.section == HOOKS_EVENTS)
			screen->focus = 0;

		if (nav.section == HOOKS_REVIEW) {
			screen->scroll = 0;
			screen->message[0] = '\0';
		}

		return cli_hooks_navigate(screen, GO_BACK);
	}

	if (key == 'r' && nav.section != HOOKS_REVIEW) {
		rc = cli_hooks_refresh(ctx, rows, screen);
		if (rc != 0)
			return rc;

		screen->message[0] = '\0';
		return NT_NAVIGATION_CHANGED;
	}

	if (screen->height < (nav.batch ? 18 : 10) || screen->width < (nav.batch ? 40 : 24) ||
	    (nav.section == HOOKS_REVIEW && screen->document_height == 0))
		return NT_NAVIGATION_UNHANDLED;

	if (nav.section != HOOKS_REVIEW && !nav.batch && key == 'e') {
		screen->scroll = 0;
		return cli_hooks_navigate(screen, SHOW_EVENTS);
	}

	if (nav.section == HOOKS_EVENTS && !nav.batch) {
		cli_hooks_choose(&screen->event_focus, CLI_HOOKS_HOOK_EVENT_COUNT,
				 screen->height - 10, key, NT_LIST_VIEW_VIM | NT_LIST_VIEW_PAGED);
		if (key == '\r' || key == '\n') {
			screen->focus = 0;
			screen->scroll = 0;
			return cli_hooks_navigate(screen, FILTER_EVENT);
		}

		return NT_NAVIGATION_CHANGED;
	}

	if (nav.batch) {
		cli_hooks_choose(&screen->choice, 3, 1, key,
				 NT_LIST_VIEW_WRAP | NT_LIST_VIEW_VIM | NT_LIST_VIEW_TAB);
		if (key == NT_KEY_PAGE_UP || key == NT_KEY_PAGE_DOWN)
			screen->scroll = (int)nt_scroll_offset_after_key(
				(size_t)screen->scroll, (size_t)cJSON_GetArraySize(*rows),
				(size_t)screen->page_size, key);

		if (key == '\r' || key == '\n') {
			if (screen->choice == 2) {
				screen->scroll = 0;
				return cli_hooks_navigate(screen, CLOSE_BATCH);
			} else {
				const cJSON *row = cJSON_GetArrayItem(*rows, 0);
				int result = runtime_hooks_manage(
					ctx->runtime,
					screen->choice == 0 ? "trust-all" : "deny-all", "workspace",
					cli_hooks_field(row, "review_digest"));

				if (result == 0)
					return NT_NAVIGATION_BACK;

				rc = cli_hooks_refresh(ctx, rows, screen);
				if (rc != 0)
					return rc;

				screen->choice = 2;
				snprintf(screen->message, sizeof(screen->message),
					 "Action failed: %s. Review the refreshed hooks.",
					 nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
						    result));
			}
		}

		return NT_NAVIGATION_CHANGED;
	}

	if (key == 'a' && nav.section != HOOKS_REVIEW && cJSON_GetArraySize(*rows) > 0) {
		rc = cli_hooks_refresh(ctx, rows, screen);
		if (rc != 0)
			return rc;

		screen->choice = 2;
		return cli_hooks_navigate(screen, REVIEW_BATCH);
	}

	const cJSON *row = cli_hooks_view_row(screen, *rows, screen->focus);
	struct cli_hooks_action items[4];
	int count = cli_hooks_actions(row, items);
	int total = cli_hooks_view_count(screen, *rows);

	if (nav.section == HOOKS_REVIEW) {
		cli_hooks_choose(&screen->choice, count, 1, key,
				 NT_LIST_VIEW_WRAP | NT_LIST_VIEW_VIM | NT_LIST_VIEW_TAB);
		if (key == NT_KEY_PAGE_UP || key == NT_KEY_PAGE_DOWN || key == NT_KEY_HOME ||
		    key == NT_KEY_END)
			screen->scroll = (int)nt_scroll_offset_after_key(
				(size_t)screen->scroll, (size_t)screen->document_lines,
				(size_t)screen->document_height, key);
	}

	if (key == '\r' || key == '\n') {
		if (!row)
			return NT_NAVIGATION_UNHANDLED;

		if (nav.section != HOOKS_REVIEW) {
			rc = cli_hooks_refresh(ctx, rows, screen);
			if (rc != 0)
				return rc;

			if (cli_hooks_view_count(screen, *rows)) {
				return cli_hooks_navigate(screen, REVIEW_HOOK);
			}
		} else if (!items[screen->choice].name) {
			screen->scroll = 0;
			return cli_hooks_navigate(screen, SHOW_HANDLERS);
		} else {
			const char *action = items[screen->choice].name;
			int project = strcmp(action, "trust-project") == 0;
			int result = runtime_hooks_manage(
				ctx->runtime, action,
				project ? "workspace" : cli_hooks_field(row, "id"),
				cli_hooks_field(row, project ? "workspace_digest" : "digest"));

			snprintf(screen->message, sizeof(screen->message),
				 result == 0 ? "Completed: %s" : "Action failed: %s",
				 result == 0 ? action
					     : nb_err_str(morph_error_descriptions(),
							  MORPH_ERROR_COUNT, result));
			rc = cli_hooks_refresh(ctx, rows, screen);
			if (rc != 0)
				return rc;
		}
	} else if (nav.section != HOOKS_REVIEW && total > 0) {
		cli_hooks_choose(&screen->focus, total, screen->page_size, key,
				 NT_LIST_VIEW_VIM | NT_LIST_VIEW_HORIZONTAL_PAGES |
					 NT_LIST_VIEW_PAGED);
		screen->scroll = 0;
	}

	return NT_NAVIGATION_CHANGED;
}

static int cli_hooks_handle(struct cli_hooks_page *s, const struct nt_event *event)
{
	int rc = cli_hooks_update(s, event);
	if (rc == NT_NAVIGATION_BACK) {
		rc = nt_view_controller_dismiss(&s->base, 0);
	} else if (rc == NT_NAVIGATION_CHANGED) {
		rc = cli_hooks_sync(s);
	}

	return rc < 0 ? rc : NT_VIEW_EVENT_STOP;
}

static int cli_hooks_key(void *data, const struct nt_view_event *event)
{
	return cli_hooks_handle(data, event->input);
}

static int cli_hooks_selected(void *data, const struct nt_view_event *event)
{
	struct cli_hooks_page *s = data;
	if (s->syncing)
		return 0;

	if (event->target == s->choices)
		s->choice = (int)event->index;
	else if (cli_hooks_navigation(s)->section == HOOKS_EVENTS)
		s->event_focus = s->first + (int)event->index;
	else
		s->focus = s->first + (int)event->index;

	if (event->type == NT_VIEW_EVENT_ACTIVATE)
		return cli_hooks_handle(s, &(struct nt_event){.type = NT_EVENT_KEY, .key = '\r'});

	return cli_hooks_sync(s);
}

static int cli_hooks_size(void *data, const struct nt_view_event *event)
{
	struct cli_hooks_page *s = data;
	if (event->target != s->root)
		return 0;

	s->width = event->rect.width > 0 ? event->rect.width : 1;
	s->height = event->rect.height;
	return cli_hooks_sync(s);
}

static int cli_hooks_back(void *data)
{
	struct cli_hooks_page *s = data;
	int rc = cli_hooks_navigate(s, SHOW_ALL_HOOKS);
	if (rc < 0)
		return rc;

	s->scroll = 0;
	s->message[0] = '\0';
	rc = cli_hooks_sync(s);
	return rc ? rc : nt_view_focus(s->root);
}

static void cli_hooks_build(struct nt_view_controller *controller, struct nt_builder *builder)
{
	struct cli_hooks_page *s = (struct cli_hooks_page *)controller;
	cli_page_layout_build(builder, &s->layout, "hooks", &nt_view_base_class,
			      cli_hooks_back, s);
	s->root = s->layout.body;
	nt_builder_fail(builder, nt_view_use_reducer(s->root,
						     &(struct nt_component_state_options){
							     .size = sizeof(s->initial),
							     .initial = &s->initial},
						     cli_hooks_reduce, &s->navigation));
	if (nt_builder_error(builder))
		return;

	s->title = nt_label_add(builder, s->root, "", NULL);
	s->subtitle = nt_label_add(builder, s->root, "", NULL);
	const struct nt_view_options dim = {.style = cli_hooks_style(HOOK_DIM)};
	s->scope = nt_label_add(builder, s->root, "", &dim);
	s->rule = nt_label_add(builder, s->root, "", &dim);
	s->heading = nt_label_add(builder, s->root, "", NULL);
	s->list = nt_list_view_add(builder, s->root, NULL, 0, 0, NULL);
	s->range = nt_label_add(builder, s->root, "", NULL);
	s->divider = nt_label_add(builder, s->root, "", &dim);
	s->viewport = nt_scroll_view_add(builder, s->root, NULL);
	s->document = nt_text_view_add(builder, s->viewport, "",
				       &(struct nt_view_options){.width = NT_FILL});
	s->choice_title =
		nt_label_add(builder, s->root, "Review decision",
			     &(struct nt_view_options){.style = cli_hooks_style(HOOK_BOLD)});
	s->choices = nt_list_view_add(builder, s->root, NULL, 0, 0, NULL);
	s->message_view = nt_label_add(builder, s->root, "", NULL);
	s->help = nt_label_add(builder, s->layout.footer, "", &dim);
	if (nt_builder_error(builder))
		return;

	struct nt_view *labels[] = {s->title,	     s->subtitle,
				    s->scope,	     s->heading,      s->range,
				    s->choice_title, s->message_view, s->help};
	for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++)
		nt_builder_fail(builder, nt_label_set_opaque(labels[i], 1));

	nt_builder_fail(builder, nt_scroll_view_set_scrollbars(s->viewport, 0));
	nt_builder_fail(builder, nt_text_view_set_fill_lines(s->document, 1));
	nt_builder_fail(builder, nt_list_view_set_row_renderer(s->list, cli_hooks_row, s));
	nt_builder_fail(builder,
			nt_list_view_set_row_renderer(s->choices, cli_hooks_choice_row, s));
	if (!nt_view_on_full(
		    s->root, NT_VIEW_EVENT_KEY,
		    &(struct nt_view_listener_options){.flags = NT_VIEW_LISTEN_CAPTURE,
						       NT_VIEW_LISTENER(cli_hooks_key, s)}) ||
	    !nt_view_on(s->root, NT_VIEW_EVENT_SIZE_CHANGED, cli_hooks_size, s))
		nt_builder_fail(builder, NB_ERR_NOMEM);

	struct nt_view *lists[] = {s->list, s->choices};
	for (size_t i = 0; i < 2; i++)
		if (!nt_view_on(lists[i], NT_VIEW_EVENT_CHANGED, cli_hooks_selected, s) ||
		    !nt_view_on(lists[i], NT_VIEW_EVENT_ACTIVATE, cli_hooks_selected, s))
			nt_builder_fail(builder, NB_ERR_NOMEM);

	nt_builder_fail(builder, cli_hooks_sync(s));
	nt_builder_fail(builder, nt_view_set_focusable(s->root, 1));
	nt_builder_fail(builder, nt_view_focus(s->root));
}

static void cli_hooks_destroy(struct nt_view_controller *controller)
{
	struct cli_hooks_page *page = (struct cli_hooks_page *)controller;
	cJSON_Delete(page->rows);
}

int cli_hooks_create_take(cJSON **rows, const char *review_id, int batch,
			  struct nt_view_controller **out)
{
	static const struct nt_view_controller_class type = {
		.superclass = &cli_application_panel_class,
		.instance_size = sizeof(struct cli_hooks_page),
		.load_view = cli_hooks_build,
		.deinit = cli_hooks_destroy,
	};
	if (!rows || !*rows)
		return NB_ERR_INVAL;

	int focus = 0;
	const cJSON *row = NULL;
	if (review_id) {
		cJSON_ArrayForEach (row, *rows) {
			if (!strcmp(cli_hooks_field(row, "id"), review_id))
				break;

			focus++;
		}

		if (!row)
			return NB_ERR_NOT_FOUND;
	}

	int rc = nt_view_controller_create(&type, out);
	if (rc)
		return rc;

	struct cli_hooks_page *page = (struct cli_hooks_page *)*out;
	page->rows = cJSON_StealOwner(rows);
	page->focus = focus;
	page->initial.batch = batch;
	page->choice = batch ? 2 : 0;
	if (row) {
		struct cli_hooks_action actions[4];
		page->initial.section = HOOKS_REVIEW;
		page->choice = cli_hooks_actions(row, actions) - 1;
	}

	nt_terminal_dimensions(STDOUT_FILENO, &page->columns, &page->height);
	page->width = page->columns > 4 ? page->columns - 4 : 1;
	return 0;
}

int cli_hooks_startup(struct cli_application *ctx)
{
	cJSON *rows NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	const cJSON *row;
	int rc;

	if (!cli_setup_controls_available(stdin, stdout))
		return 0;

	rc = runtime_hooks_snapshot_copy(ctx->runtime, &rows);
	if (rc != 0)
		return rc;

	cJSON_ArrayForEach (row, rows) {
		const char *state = cli_hooks_field(row, "state");

		if (strcmp(state, "needs_review") == 0 ||
		    strcmp(state, "changed / needs_review") == 0 ||
		    strcmp(state, "project_untrusted") == 0) {
			struct nt_view_controller *page NB_CLEANUP(
				cli_application_controller_cleanup) = NULL;
			rc = cli_hooks_create_take(&rows, NULL, 1, &page);
			return rc ? rc : cli_application_show_page(ctx, &page, 0);
		}
	}

	return rc;
}
