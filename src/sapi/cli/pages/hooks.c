#include "sapi/cli/pages/hooks.h"
#include "cJSON.h"
#include <morph/tui/components.h>
#include "sapi/cli/application.h"
#include "sapi/cli/pages/setup/controls.h"
#include "runtime/runtime.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include "nb/vec.h"
#include "nb/buf.h"
#include "errors.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CLI_HOOKS_UI_TEXT "\033[0m"
#define CLI_HOOKS_UI_DIM "\033[0;2m"
#define CLI_HOOKS_UI_CYAN "\033[0;38;2;71;211;230m"
#define CLI_HOOKS_UI_GREEN "\033[0;38;2;124;217;167m"
#define CLI_HOOKS_UI_AMBER "\033[0;38;2;226;185;91m"
#define CLI_HOOKS_UI_RED "\033[0;38;2;240;163;163m"
#define CLI_HOOKS_UI_ADD "\033[0;38;2;160;227;182m"
#define CLI_HOOKS_UI_SELECTED "\033[0;1;36m"
#define CLI_HOOKS_UI_BAR "\033[0;1;36m"
#define CLI_HOOKS_UI_RULE "\033[0;2m"

struct cli_hooks_hook_line {
	char *text;
	const char *style;
};

struct cli_hooks_hook_screen {
	nb_buf frame;
	int height;
	int columns;
	int width;
	int focus;
	int review;
	int batch;
	int events;
	int event_focus;
	int event_filter;
	int scroll;
	int choice;
	int page_size;
	int document_height;
	int document_lines;
	char message[256];
};

struct cli_hooks_hook_action {
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

static const char *cli_hooks_state_style(const cJSON *row)
{
	const char *state = cli_hooks_field(row, "state");

	if (strcmp(state, "trusted") == 0)
		return CLI_HOOKS_UI_GREEN;
	if (strcmp(state, "disabled") == 0 || strcmp(state, "off") == 0)
		return CLI_HOOKS_UI_DIM;
	return CLI_HOOKS_UI_AMBER;
}

static const char *cli_hooks_selected_state_style(const cJSON *row)
{
	const char *state = cli_hooks_field(row, "state");

	if (strcmp(state, "trusted") == 0)
		return CLI_HOOKS_UI_GREEN "\033[1m";
	if (strcmp(state, "disabled") == 0 || strcmp(state, "off") == 0)
		return CLI_HOOKS_UI_DIM "\033[1m";
	return CLI_HOOKS_UI_AMBER "\033[1m";
}

/* Only trusted positioning/styles bypass the CLI output filter. */
static void cli_hooks_cell(struct cli_hooks_hook_screen *screen, int row, int column, int width,
			   const char *text, const char *style)
{
	struct morph_tui_terminal_rect area = {1, 1, screen->columns, screen->height};

	(void)morph_tui_draw_region_cell(&screen->frame, area, row - 1, column - 1, width, text,
					 style);
}

static void cli_hooks_rule(struct cli_hooks_hook_screen *screen, int row)
{
	if (row < 1 || row > screen->height)
		return;
	nb_buf_printf(&screen->frame, "\033[%d;3H%s", row, CLI_HOOKS_UI_RULE);
	for (int i = 0; i < screen->width; i++)
		nb_buf_append_cstr(&screen->frame, "─");
}

static void cli_hooks_selected(struct cli_hooks_hook_screen *screen, int row, const char *text)
{
	cli_hooks_cell(screen, row, 3, screen->width, text, CLI_HOOKS_UI_SELECTED "\033[1m");
	cli_hooks_cell(screen, row, 2, 1, "▏", CLI_HOOKS_UI_BAR);
}

static int cli_hooks_actions(const cJSON *row, struct cli_hooks_hook_action items[4])
{
	const char *state = cli_hooks_field(row, "state");
	int count = 0;

	if (strcmp(state, "trusted") == 0) {
		items[count++] = (struct cli_hooks_hook_action){"revoke", "Revoke trust"};
		items[count++] = (struct cli_hooks_hook_action){"disable", "Disable hook"};
	} else if (strcmp(state, "disabled") == 0) {
		items[count++] = (struct cli_hooks_hook_action){"enable", "Enable hook"};
		items[count++] = (struct cli_hooks_hook_action){"revoke", "Revoke trust"};
	} else if (strcmp(state, "project_untrusted") == 0) {
		items[count++] =
			(struct cli_hooks_hook_action){"trust-project", "Trust this workspace"};
		items[count++] = (struct cli_hooks_hook_action){"disable", "Disable hook"};
	} else if (strcmp(state, "needs_review") == 0 ||
		   strcmp(state, "changed / needs_review") == 0) {
		items[count++] =
			(struct cli_hooks_hook_action){"trust", "Trust current definition"};
		items[count++] = (struct cli_hooks_hook_action){"disable", "Disable hook"};
	}
	items[count++] = (struct cli_hooks_hook_action){
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

static int cli_hooks_view_count(const struct cli_hooks_hook_screen *screen, const cJSON *rows)
{
	const cJSON *row;
	int count = 0;

	if (!screen->event_filter)
		return cJSON_GetArraySize(rows);
	cJSON_ArrayForEach (row, rows) {
		if (strcmp(cli_hooks_field(row, "event"),
			   cli_hooks_event_name(screen->event_filter - 1)) == 0)
			count++;
	}
	return count;
}

static const cJSON *cli_hooks_view_row(const struct cli_hooks_hook_screen *screen,
				       const cJSON *rows, int index)
{
	const cJSON *row;

	if (!screen->event_filter)
		return cJSON_GetArrayItem(rows, index);
	cJSON_ArrayForEach (row, rows) {
		if (strcmp(cli_hooks_field(row, "event"),
			   cli_hooks_event_name(screen->event_filter - 1)) == 0 &&
		    index-- == 0)
			return row;
	}
	return NULL;
}

static void cli_hooks_events_frame(struct cli_hooks_hook_screen *screen, const cJSON *rows)
{
	char label[160];
	int page = screen->height - 10;
	int start = screen->event_focus / page * page;
	int end = start + page < CLI_HOOKS_HOOK_EVENT_COUNT ? start + page
							    : CLI_HOOKS_HOOK_EVENT_COUNT;

	cli_hooks_cell(screen, 3, 3, screen->width, "Lifecycle events",
		       CLI_HOOKS_UI_TEXT "\033[1m");
	cli_hooks_cell(screen, 4, 3, screen->width,
		       "Inspect installed, active, and pending hooks by event.", CLI_HOOKS_UI_DIM);
	cli_hooks_rule(screen, 5);
	cli_hooks_cell(screen, 6, 3, screen->width,
		       "EVENT                  INSTALLED  ACTIVE  REVIEW", CLI_HOOKS_UI_DIM);
	for (int i = start; i < end; i++) {
		const cJSON *row;
		int installed = 0, active = 0, pending = 0;

		cJSON_ArrayForEach (row, rows) {
			if (strcmp(cli_hooks_field(row, "event"), cli_hooks_event_name(i)) != 0)
				continue;
			installed++;
			active += strcmp(cli_hooks_field(row, "state"), "trusted") == 0;
			pending += strcmp(cli_hooks_field(row, "state"), "needs_review") == 0 ||
				   strcmp(cli_hooks_field(row, "state"),
					  "changed / needs_review") == 0 ||
				   strcmp(cli_hooks_field(row, "state"), "project_untrusted") == 0;
		}
		snprintf(label, sizeof(label), "%-22s %-10d %-7d %d", cli_hooks_event_name(i),
			 installed, active, pending);
		if (i == screen->event_focus)
			cli_hooks_selected(screen, 7 + i - start, label);
		else
			cli_hooks_cell(screen, 7 + i - start, 3, screen->width, label,
				       CLI_HOOKS_UI_TEXT);
	}
	cli_hooks_cell(screen, screen->height, 3, screen->width,
		       "↑↓ Select  Enter Handlers  r Refresh  Esc All hooks", CLI_HOOKS_UI_DIM);
}

static int cli_hooks_add_line(nb_vec *lines, const char *text, const char *style, int width)
{
	char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		text, strlen(text), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);

	if (!safe)
		return NB_ERR_NOMEM;
	const char *cursor = safe;
	int rc = 0;

	do {
		const char *end =
			morph_terminal_advance_display_width_borrow(cursor, (size_t)width);
		if (*cursor && end == cursor)
			end = nb_utf8_advance(cursor, 1);
		size_t length = (size_t)(end - cursor);
		char *copy NB_CLEANUP(nb_charp_cleanup) = malloc(length + 1);
		struct cli_hooks_hook_line *line = copy ? nb_vec_push(lines) : NULL;

		if (!copy || !line) {
			rc = (NB_ERR_NOMEM);
			break;
		}
		memcpy(copy, cursor, length);
		copy[length] = '\0';
		*line = (struct cli_hooks_hook_line){nb_charp_steal(&copy), style};
		cursor = end;
	} while (*cursor);
	return rc;
}

static int cli_hooks_detail_field(nb_vec *lines, nb_buf *buffer, const char *label,
				  const char *value, int width)
{
	nb_buf_clear(buffer);
	int rc =
		(nb_buf_printf(buffer, "%-13s %s", label, *value ? value : "—") ? 0 : NB_ERR_NOMEM);

	return rc == 0 ? cli_hooks_add_line(lines, nb_buf_cstr(buffer), CLI_HOOKS_UI_TEXT, width)
		       : rc;
}

static int cli_hooks_diff_property(nb_vec *lines, nb_buf *buffer, const cJSON *value,
				   const char *prefix, const char *style, int width)
{
	char *json NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(value);
	if (!json)
		return NB_ERR_NOMEM;
	nb_buf_clear(buffer);
	int rc = (nb_buf_printf(buffer, "%s  \"%s\": %s%s", prefix, value->string, json,
				value->next ? "," : "")
			  ? 0
			  : NB_ERR_NOMEM);
	return rc == 0 ? cli_hooks_add_line(lines, nb_buf_cstr(buffer), style, width) : rc;
}

static int cli_hooks_definition_lines(nb_vec *lines, nb_buf *buffer, const cJSON *row, int width)
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
				    CLI_HOOKS_UI_TEXT "\033[1m", width);

	if (rc == 0)
		rc = cli_hooks_add_line(lines, "{", CLI_HOOKS_UI_DIM, width);
	cJSON_ArrayForEach (value, previous) {
		if (rc != 0)
			break;
		if (!cJSON_HasObjectItem(current, value->string))
			rc = cli_hooks_diff_property(lines, buffer, value, "-", CLI_HOOKS_UI_RED,
						     width);
	}
	cJSON_ArrayForEach (value, current) {
		if (rc != 0)
			break;
		const cJSON *old = cJSON_GetObjectItemCaseSensitive(previous, value->string);
		int changed = previous && !cJSON_Compare(old, value, 1);

		if (changed && old)
			rc = cli_hooks_diff_property(lines, buffer, old, "-", CLI_HOOKS_UI_RED,
						     width);
		if (rc == 0)
			rc = cli_hooks_diff_property(lines, buffer, value, changed ? "+" : " ",
						     changed ? CLI_HOOKS_UI_ADD : CLI_HOOKS_UI_TEXT,
						     width);
	}
	if (rc == 0)
		rc = cli_hooks_add_line(lines, "}", CLI_HOOKS_UI_DIM, width);
	return rc;
}

static int cli_hooks_document(nb_vec *lines, const cJSON *row, int width, int review)
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
			rc = cli_hooks_add_line(lines, nb_buf_cstr(&buffer),
						CLI_HOOKS_UI_TEXT "\033[1m", width);
	}

	for (size_t i = 0; rc == 0 && i < sizeof(keys) / sizeof(keys[0]); i++)
		rc = cli_hooks_detail_field(lines, &buffer, labels[i],
					    cli_hooks_field(row, keys[i]), width);
	if (rc == 0 && *cli_hooks_field(row, "diagnostic"))
		rc = cli_hooks_detail_field(lines, &buffer, "Unavailable",
					    cli_hooks_field(row, "diagnostic"), width);
	if (rc == 0 && *cli_hooks_field(row, "statusMessage"))
		rc = cli_hooks_detail_field(lines, &buffer, "While running",
					    cli_hooks_field(row, "statusMessage"), width);
	if (rc == 0) {
		const cJSON *timeout = cJSON_GetObjectItemCaseSensitive(row, "timeout");
		char number[32];

		snprintf(number, sizeof(number), "%ds",
			 cJSON_IsNumber(timeout) ? timeout->valueint : 0);
		rc = cli_hooks_detail_field(lines, &buffer, "Timeout", number, width);
	}
	if (rc == 0 && review)
		rc = cli_hooks_add_line(lines, "", CLI_HOOKS_UI_DIM, width);
	if (rc == 0 && review)
		rc = cli_hooks_definition_lines(lines, &buffer, row, width);
	if (rc == 0)
		rc = cli_hooks_add_line(lines, "", CLI_HOOKS_UI_DIM, width);
	if (rc == 0)
		rc = cli_hooks_add_line(lines, "Last execution", CLI_HOOKS_UI_TEXT "\033[1m",
					width);
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
			rc = cli_hooks_add_line(lines, nb_buf_cstr(&buffer), CLI_HOOKS_UI_DIM,
						width);
		if (rc == 0 && *cli_hooks_field(last, "error"))
			rc = cli_hooks_detail_field(lines, &buffer, "Error",
						    cli_hooks_field(last, "error"), width);
		if (rc == 0 && *cli_hooks_field(last, "stderr"))
			rc = cli_hooks_detail_field(lines, &buffer, "Stderr",
						    cli_hooks_field(last, "stderr"), width);
	} else if (rc == 0) {
		rc = cli_hooks_add_line(lines, "Not run", CLI_HOOKS_UI_DIM, width);
	}
	if (rc == 0 && review)
		rc = cli_hooks_add_line(
			lines, "Trust covers this definition, not mutable script contents.",
			CLI_HOOKS_UI_DIM, width);
	if (rc == 0 && strcmp(cli_hooks_field(row, "state"), "project_untrusted") == 0)
		rc = cli_hooks_add_line(lines,
					"Review this workspace path before trusting it; then "
					"review the hook definition.",
					CLI_HOOKS_UI_AMBER, width);
	return rc;
}

static void cli_hooks_list_frame(struct cli_hooks_hook_screen *screen, const cJSON *rows)
{
	int count = cli_hooks_view_count(screen, rows);
	int trusted = 0, disabled = 0, review = 0;
	const cJSON *row;
	char label[128];

	cJSON_ArrayForEach (row, rows) {
		trusted += strcmp(cli_hooks_field(row, "state"), "trusted") == 0;
		disabled += strcmp(cli_hooks_field(row, "state"), "disabled") == 0;
		review += strcmp(cli_hooks_field(row, "state"), "needs_review") == 0 ||
			  strcmp(cli_hooks_field(row, "state"), "changed / needs_review") == 0 ||
			  strcmp(cli_hooks_field(row, "state"), "project_untrusted") == 0;
	}
	snprintf(label, sizeof(label), "Lifecycle hooks%s%s", screen->event_filter ? " · " : "",
		 screen->event_filter ? cli_hooks_event_name(screen->event_filter - 1) : "");
	cli_hooks_cell(screen, 3, 3, screen->width, label, CLI_HOOKS_UI_TEXT "\033[1m");
	snprintf(label, sizeof(label), "%d trusted · %d need review · %d disabled", trusted, review,
		 disabled);
	if (count > trusted + disabled + review)
		snprintf(label, sizeof(label),
			 "%d trusted · %d need review · %d disabled · %d unavailable", trusted,
			 review, disabled, count - trusted - disabled - review);
	cli_hooks_cell(screen, 4, 3, screen->width, label, CLI_HOOKS_UI_DIM);
	cli_hooks_rule(screen, 5);
	int state_width = screen->width >= 68 ? 22 : screen->width >= 40 ? 16 : 0;
	int source_width = screen->width >= 90 ? 10 : 0;
	int event_width = screen->width >= 68 ? 22 : screen->width >= 40 ? 20 : screen->width;
	struct morph_tui_terminal_rect table = {3, 6, screen->width, 1};
	struct morph_tui_terminal_rect event_area =
		morph_tui_terminal_rect_take_left(&table, event_width);
	int command_column = table.x;
	int command_width = table.width - state_width - source_width - 2;

	(void)morph_tui_terminal_rect_take_left(&table, command_width + 2);
	struct morph_tui_terminal_rect source_area =
		morph_tui_terminal_rect_take_left(&table, source_width);
	int source_column = source_area.x;
	int state_column = table.x;

	cli_hooks_cell(screen, event_area.y, event_area.x, event_area.width, "EVENT",
		       CLI_HOOKS_UI_DIM);
	if (command_width > 4)
		cli_hooks_cell(screen, 6, command_column, command_width, "COMMAND",
			       CLI_HOOKS_UI_DIM);
	if (source_width)
		cli_hooks_cell(screen, 6, source_column, source_width, "SOURCE", CLI_HOOKS_UI_DIM);
	if (state_width)
		cli_hooks_cell(screen, 6, state_column, state_width, "STATE", CLI_HOOKS_UI_DIM);
	int start = screen->focus / screen->page_size * screen->page_size;
	int end = count - start > screen->page_size ? start + screen->page_size : count;

	for (int i = start; i < end; i++) {
		row = cli_hooks_view_row(screen, rows, i);
		int y = 7 + i - start;
		int focus = i == screen->focus;
		const char *style = focus ? CLI_HOOKS_UI_SELECTED "\033[1m" : CLI_HOOKS_UI_TEXT;

		if (focus)
			cli_hooks_selected(screen, y, "");
		cli_hooks_cell(screen, y, 3, event_width, cli_hooks_field(row, "event"), style);
		if (command_width > 4)
			cli_hooks_cell(screen, y, command_column, command_width,
				       cli_hooks_field(row, "command"), style);
		if (source_width) {
			int project =
				cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "project"));

			cli_hooks_cell(screen, y, source_column, source_width,
				       project ? "Project" : "User", style);
		}
		if (state_width) {
			cli_hooks_cell(screen, y, state_column, state_width,
				       cli_hooks_state_label(row),
				       focus ? cli_hooks_selected_state_style(row)
					     : cli_hooks_state_style(row));
		}
	}
	int summary = 7 + end - start;
	snprintf(label, sizeof(label), "Showing %d–%d of %d", count ? start + 1 : 0, end, count);
	cli_hooks_cell(screen, summary, 3, screen->width, label, CLI_HOOKS_UI_DIM);
	if (!count)
		cli_hooks_cell(screen, 7, 3, screen->width, "No hooks configured.",
			       CLI_HOOKS_UI_DIM);
}

static void cli_hooks_render_batch(struct cli_hooks_hook_screen *screen, const cJSON *rows)
{
	int count = cJSON_GetArraySize(rows);
	int visible = (screen->height - 16) / 3;
	const char *choices[] = {"Trust all current hooks and remember",
				 "Reject all current hooks and remember",
				 "Review individually / keep unchanged"};
	char label[128];

	if (visible < 1)
		visible = 1;
	if (screen->scroll > count - visible)
		screen->scroll = count > visible ? count - visible : 0;
	screen->page_size = visible;
	cli_hooks_cell(screen, 3, 3, screen->width, "Trust lifecycle hooks?",
		       CLI_HOOKS_UI_TEXT "\033[1m");
	cli_hooks_cell(screen, 4, 3, screen->width,
		       cli_hooks_field(cJSON_GetArrayItem(rows, 0), "cwd"), CLI_HOOKS_UI_DIM);
	cli_hooks_cell(screen, 5, 3, screen->width,
		       "User hooks: all workspaces. Project hooks: this workspace only.",
		       CLI_HOOKS_UI_DIM);
	cli_hooks_rule(screen, 6);
	for (int i = 0; i < visible && screen->scroll + i < count; i++) {
		const cJSON *row = cJSON_GetArrayItem(rows, screen->scroll + i);
		int y = 7 + i * 3;

		snprintf(label, sizeof(label), "%s · %s", cli_hooks_field(row, "event"),
			 cli_hooks_state_label(row));
		cli_hooks_cell(screen, y, 3, screen->width, label, cli_hooks_state_style(row));
		cli_hooks_cell(screen, y + 1, 5, screen->width - 2, cli_hooks_field(row, "source"),
			       CLI_HOOKS_UI_DIM);
		cli_hooks_cell(screen, y + 2, 5, screen->width - 2, cli_hooks_field(row, "command"),
			       CLI_HOOKS_UI_TEXT);
	}
	snprintf(label, sizeof(label), "Showing %d–%d of %d · PgUp/PgDn to scroll",
		 count ? screen->scroll + 1 : 0,
		 screen->scroll + visible < count ? screen->scroll + visible : count, count);
	cli_hooks_cell(screen, screen->height - 8, 3, screen->width, label, CLI_HOOKS_UI_DIM);
	cli_hooks_rule(screen, screen->height - 7);
	for (int i = 0; i < 3; i++) {
		if (i == screen->choice)
			cli_hooks_selected(screen, screen->height - 6 + i, choices[i]);
		else
			cli_hooks_cell(screen, screen->height - 6 + i, 3, screen->width, choices[i],
				       CLI_HOOKS_UI_TEXT);
	}
	cli_hooks_cell(screen, screen->height - 2, 3, screen->width, screen->message,
		       CLI_HOOKS_UI_AMBER);
	cli_hooks_cell(screen, screen->height, 3, screen->width,
		       "↑↓ Choose  Enter Confirm  Esc Keep unchanged", CLI_HOOKS_UI_DIM);
}

static void cli_hooks_lines_cleanup(nb_vec *lines)
{
	for (size_t i = 0; i < lines->len; i++) {
		struct cli_hooks_hook_line *line = nb_vec_at(lines, i);

		free(line->text);
	}
	nb_vec_cleanup(lines);
}

static int cli_hooks_render(struct cli_hooks_hook_screen *screen, const cJSON *rows)
{
	const cJSON *row = cli_hooks_view_row(screen, rows, screen->focus);
	int height = screen->height;
	int detail_start;
	struct cli_hooks_hook_action items[4];
	int action_count = cli_hooks_actions(row, items);
	nb_vec lines NB_CLEANUP(cli_hooks_lines_cleanup) = {0};

	struct cli_hooks_hook_line element;
	int rc = (nb_vec_init(&lines, sizeof(element)),
		  nb_vec_reserve(&lines, 32) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;

	if (!screen->frame.ptr)
		rc = (nb_buf_init(&screen->frame),
		      nb_buf_reserve(&screen->frame, BUFSIZ) ? 0 : NB_ERR_NOMEM);
	else
		nb_buf_clear(&screen->frame);
	if (rc == 0)
		rc = (nb_buf_printf(&screen->frame, "\033[?2026h%s\033[H\033[J", CLI_HOOKS_UI_TEXT)
			      ? 0
			      : NB_ERR_NOMEM);
	cli_hooks_cell(screen, 1, 3, 5, "morph", CLI_HOOKS_UI_CYAN);
	cli_hooks_cell(screen, 1, 9, screen->width - 6,
		       screen->review ? "/ hooks / review" : "/ hooks", CLI_HOOKS_UI_DIM);
	if (height < (screen->batch ? 20 : 12) || screen->width < (screen->batch ? 40 : 24)) {
		cli_hooks_cell(screen, 3, 3, screen->width, "Resize terminal to view hooks.",
			       CLI_HOOKS_UI_AMBER);
		cli_hooks_cell(screen, height, 3, screen->width, "Esc Back / q Close",
			       CLI_HOOKS_UI_DIM);
		screen->document_height = 0;
	} else if (screen->batch) {
		cli_hooks_render_batch(screen, rows);
	} else if (screen->events) {
		cli_hooks_events_frame(screen, rows);
	} else if (cli_hooks_view_count(screen, rows) == 0) {
		screen->page_size = 1;
		screen->document_height = 0;
		screen->document_lines = 0;
		cli_hooks_cell(screen, 3, 3, screen->width,
			       screen->event_filter ? cli_hooks_event_name(screen->event_filter - 1)
						    : "Lifecycle hooks",
			       CLI_HOOKS_UI_TEXT "\033[1m");
		cli_hooks_cell(screen, 6, 3, screen->width,
			       screen->event_filter ? "No hooks configured for this event."
						    : "No hooks configured.",
			       CLI_HOOKS_UI_TEXT);
		if (height >= 14) {
			cli_hooks_cell(screen, 8, 3, screen->width,
				       "Add hook definitions to:", CLI_HOOKS_UI_DIM);
			cli_hooks_cell(screen, 9, 3, screen->width, "~/.morph/hooks.json   (user)",
				       CLI_HOOKS_UI_DIM);
			cli_hooks_cell(screen, 10, 3, screen->width,
				       ".morph/hooks.json     (project)", CLI_HOOKS_UI_DIM);
		}
		cli_hooks_cell(screen, height, 3, screen->width, "e Events  r Refresh  Esc Back",
			       CLI_HOOKS_UI_DIM);
	} else {
		if (screen->review) {
			cli_hooks_cell(screen, 3, 3, screen->width, cli_hooks_field(row, "event"),
				       CLI_HOOKS_UI_TEXT "\033[1m");
			cli_hooks_cell(screen, 4, 3, screen->width, cli_hooks_state_label(row),
				       cli_hooks_state_style(row));
			cli_hooks_rule(screen, 5);
			detail_start = 6;
		} else {
			screen->page_size = height >= 24 ? (height - 20) / 2 : height - 10;
			if (screen->page_size < 1)
				screen->page_size = 1;
			int total = cli_hooks_view_count(screen, rows);
			if (total > 0 && screen->page_size > total)
				screen->page_size = total;
			cli_hooks_list_frame(screen, rows);
			int start = screen->focus / screen->page_size * screen->page_size;
			int visible = total - start < screen->page_size ? total - start
									: screen->page_size;
			detail_start = 9 + visible;
			if (height < 24)
				detail_start = height - 2;
			else
				cli_hooks_rule(screen, detail_start - 1);
		}
		int bottom = screen->review ? height - action_count - 5 : height - 3;
		screen->document_height = bottom - detail_start + 1;
		if (screen->document_height < 0)
			screen->document_height = 0;
		if (rc == 0 && row)
			rc = cli_hooks_document(&lines, row, screen->width, screen->review);
		screen->document_lines = lines.len > INT_MAX ? INT_MAX : (int)lines.len;
		int max_scroll = screen->document_lines - screen->document_height;
		if (max_scroll < 0)
			max_scroll = 0;
		if (screen->scroll > max_scroll)
			screen->scroll = max_scroll;
		for (int i = 0; rc == 0 && i < screen->document_height; i++) {
			struct cli_hooks_hook_line *line =
				nb_vec_at(&lines, (size_t)(screen->scroll + i));
			if (!line)
				break;
			cli_hooks_cell(screen, detail_start + i, 3, screen->width, line->text,
				       line->style);
		}
		if (screen->review) {
			int title = height - action_count - 3;

			cli_hooks_rule(screen, title - 1);
			cli_hooks_cell(screen, title, 3, screen->width, "Review decision",
				       CLI_HOOKS_UI_TEXT "\033[1m");
			for (int i = 0; i < action_count; i++) {
				if (i == screen->choice)
					cli_hooks_selected(screen, title + 1 + i, items[i].label);
				else
					cli_hooks_cell(screen, title + 1 + i, 3, screen->width,
						       items[i].label, CLI_HOOKS_UI_TEXT);
			}
		}
		cli_hooks_cell(screen, height, 3, screen->width,
			       screen->review
				       ? "↑↓ Choose  Enter Confirm  PgUp/PgDn Scroll  Esc Back"
			       : cli_hooks_view_count(screen, rows) > screen->page_size
				       ? "↑↓ Select  Enter Review  e Events  a Review all  ←→ Page "
					 " r Refresh  Esc Close"
				       : "↑↓ Select  Enter Review  e Events  a Review all  r "
					 "Refresh  Esc Close",
			       CLI_HOOKS_UI_DIM);
	}
	if (!screen->batch && *screen->message)
		cli_hooks_cell(screen, height - 1, 3, screen->width, screen->message,
			       CLI_HOOKS_UI_AMBER);
	else if (screen->review && screen->document_height > 0 &&
		 screen->document_lines > screen->document_height) {
		char status[80];

		snprintf(status, sizeof(status), "Lines %d–%d / %d · PgUp/PgDn to read more",
			 screen->scroll + 1, screen->scroll + screen->document_height,
			 screen->document_lines);
		cli_hooks_cell(screen, height - 1, 3, screen->width, status, CLI_HOOKS_UI_DIM);
	}
	if (rc == 0)
		rc = (nb_buf_append_cstr(&screen->frame, "\033[0m\033[?2026l") ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = morph_tui_draw_write(stdout, &screen->frame);
	return rc;
}

static int cli_hooks_refresh(struct cli_application *ctx, cJSON **rows,
			     struct cli_hooks_hook_screen *screen)
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
		screen->review = 0;
	}
	struct cli_hooks_hook_action items[4];
	screen->choice =
		cli_hooks_actions(cli_hooks_view_row(screen, fresh, screen->focus), items) - 1;
	screen->scroll = 0;
	return 0;
}

struct cli_hooks_hook_controller {
	struct cli_application *ctx;
	cJSON **rows;
	cJSON *owned_rows;
	struct cli_hooks_hook_screen screen;
};

static void cli_hooks_hook_choose(int *index, int count, int page, int key, unsigned flags)
{
	struct morph_tui_widget_menu_state menu = {0};

	menu.list.index = (size_t)*index;
	menu.list.flags = flags;
	if ((flags & MORPH_TUI_WIDGET_LIST_WRAP) && key != MORPH_TUI_KEY_UP &&
	    key != MORPH_TUI_KEY_DOWN && key != 'k' && key != 'j' && key != '\t')
		key = 0;
	morph_tui_widget_list_state_layout(&menu.list, (size_t)count, page > 0 ? (size_t)page : 1);
	(void)morph_tui_widget_menu_state_update(&menu, key);
	*index = (int)menu.list.index;
}

static int cli_hooks_draw(void *data, const struct morph_tui_screen *terminal)
{
	struct cli_hooks_hook_controller *controller = data;
	struct cli_hooks_hook_screen *screen = &controller->screen;
	struct morph_tui_terminal_rect body = morph_tui_terminal_rect_inset(terminal->area, 2, 0);

	screen->height = terminal->area.height;
	screen->columns = terminal->area.width;
	screen->width = body.width > 0 ? body.width : 1;
	return cli_hooks_render(screen, *controller->rows);
}

static int cli_hooks_update(void *data, const struct morph_tui_event *event)
{
	struct cli_hooks_hook_controller *controller = data;
	struct cli_hooks_hook_screen *screen = &controller->screen;
	struct cli_application *ctx = controller->ctx;
	cJSON **rows = controller->rows;

	if (event->type != MORPH_TUI_EVENT_KEY || event->pasting)
		return MORPH_TUI_SCREEN_CONTINUE;
	int key = event->key;
	int rc;

	if (key == 3 || key == 4 || key == 'q')
		return MORPH_TUI_SCREEN_CLOSE;
	if (key == 27) {
		if (!screen->review) {
			if (screen->events) {
				screen->events = 0;
				screen->event_filter = 0;
				screen->focus = 0;
			} else if (screen->event_filter) {
				screen->events = 1;
			} else {
				return MORPH_TUI_SCREEN_CLOSE;
			}
			return MORPH_TUI_SCREEN_REDRAW;
		}
		screen->review = 0;
		screen->scroll = 0;
		screen->message[0] = '\0';
		return MORPH_TUI_SCREEN_REDRAW;
	}
	if (key == 'r' && !screen->review) {
		rc = cli_hooks_refresh(ctx, rows, screen);
		if (rc != 0)
			return rc;
		screen->message[0] = '\0';
		return MORPH_TUI_SCREEN_REDRAW;
	}
	if (screen->height < (screen->batch ? 20 : 12) ||
	    screen->width < (screen->batch ? 40 : 24) ||
	    (screen->review && screen->document_height == 0))
		return MORPH_TUI_SCREEN_CONTINUE;
	if (!screen->review && !screen->batch && key == 'e') {
		screen->events = 1;
		screen->scroll = 0;
		return MORPH_TUI_SCREEN_REDRAW;
	}
	if (screen->events && !screen->batch) {
		cli_hooks_hook_choose(&screen->event_focus, CLI_HOOKS_HOOK_EVENT_COUNT,
				      screen->height - 10, key,
				      MORPH_TUI_WIDGET_LIST_VIM | MORPH_TUI_WIDGET_LIST_PAGED);
		if (key == '\r' || key == '\n') {
			screen->event_filter = screen->event_focus + 1;
			screen->focus = 0;
			screen->events = 0;
			screen->scroll = 0;
		}
		return MORPH_TUI_SCREEN_REDRAW;
	}
	if (screen->batch) {
		cli_hooks_hook_choose(&screen->choice, 3, 1, key,
				      MORPH_TUI_WIDGET_LIST_WRAP | MORPH_TUI_WIDGET_LIST_VIM |
					      MORPH_TUI_WIDGET_LIST_TAB);
		if (key == MORPH_TUI_KEY_PAGE_UP || key == MORPH_TUI_KEY_PAGE_DOWN)
			screen->scroll = (int)morph_tui_widget_scroll_update(
				(size_t)screen->scroll, (size_t)cJSON_GetArraySize(*rows),
				(size_t)screen->page_size, key);
		if (key == '\r' || key == '\n') {
			if (screen->choice == 2) {
				screen->batch = 0;
				screen->scroll = 0;
			} else {
				const cJSON *row = cJSON_GetArrayItem(*rows, 0);
				int result = runtime_hooks_manage(
					ctx->runtime,
					screen->choice == 0 ? "trust-all" : "deny-all", "workspace",
					cli_hooks_field(row, "review_digest"));

				if (result == 0)
					return MORPH_TUI_SCREEN_CLOSE;
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
		return MORPH_TUI_SCREEN_REDRAW;
	}
	if (key == 'a' && !screen->review && cJSON_GetArraySize(*rows) > 0) {
		rc = cli_hooks_refresh(ctx, rows, screen);
		if (rc != 0)
			return rc;
		screen->batch = 1;
		screen->choice = 2;
		return MORPH_TUI_SCREEN_REDRAW;
	}
	const cJSON *row = cli_hooks_view_row(screen, *rows, screen->focus);
	struct cli_hooks_hook_action items[4];
	int count = cli_hooks_actions(row, items);
	int total = cli_hooks_view_count(screen, *rows);

	if (screen->review) {
		cli_hooks_hook_choose(&screen->choice, count, 1, key,
				      MORPH_TUI_WIDGET_LIST_WRAP | MORPH_TUI_WIDGET_LIST_VIM |
					      MORPH_TUI_WIDGET_LIST_TAB);
		if (key == MORPH_TUI_KEY_PAGE_UP || key == MORPH_TUI_KEY_PAGE_DOWN ||
		    key == MORPH_TUI_KEY_HOME || key == MORPH_TUI_KEY_END)
			screen->scroll = (int)morph_tui_widget_scroll_update(
				(size_t)screen->scroll, (size_t)screen->document_lines,
				(size_t)screen->document_height, key);
	}
	if (key == '\r' || key == '\n') {
		if (!row)
			return MORPH_TUI_SCREEN_CONTINUE;
		if (!screen->review) {
			rc = cli_hooks_refresh(ctx, rows, screen);
			if (rc != 0)
				return rc;
			screen->review = cli_hooks_view_count(screen, *rows) > 0;
		} else if (!items[screen->choice].name) {
			screen->review = 0;
			screen->scroll = 0;
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
	} else if (!screen->review && total > 0) {
		cli_hooks_hook_choose(&screen->focus, total, screen->page_size, key,
				      MORPH_TUI_WIDGET_LIST_VIM |
					      MORPH_TUI_WIDGET_LIST_HORIZONTAL_PAGES |
					      MORPH_TUI_WIDGET_LIST_PAGED);
		screen->scroll = 0;
	}
	return MORPH_TUI_SCREEN_REDRAW;
}

static void cli_hooks_close(void *data, int result)
{
	struct cli_hooks_hook_controller *controller = data;

	(void)result;
	nb_buf_cleanup(&controller->screen.frame);
	cJSON_Delete(controller->owned_rows);
	free(controller);
}

static int cli_hooks_present(struct cli_application *ctx, cJSON **rows, const char *review_id,
			     int batch)
{
	struct cli_hooks_hook_controller controller = {
		.ctx = ctx,
		.rows = rows,
		.screen = {.batch = batch, .choice = batch ? 2 : 0, .page_size = 1}};
	struct cli_hooks_hook_screen *screen = &controller.screen;
	struct morph_tui_screen terminal = {0};
	const struct morph_tui_screen_ops ops = {.update = cli_hooks_update,
						 .draw = cli_hooks_draw};

	if (review_id) {
		const cJSON *row;
		int found = 0;

		cJSON_ArrayForEach (row, *rows) {
			if (strcmp(cli_hooks_field(row, "id"), review_id) == 0) {
				found = 1;
				break;
			}
			screen->focus++;
		}
		if (!found)
			return NB_ERR_NOT_FOUND;
		screen->review = 1;
		struct cli_hooks_hook_action items[4];

		screen->choice = cli_hooks_actions(row, items) - 1;
	}
	if (cli_application_tui_borrow(ctx)) {
		struct cli_hooks_hook_controller *owned = calloc(1, sizeof(*owned));

		if (!owned)
			return NB_ERR_NOMEM;
		*owned = controller;
		owned->owned_rows = cJSON_Duplicate(*rows, 1);
		owned->rows = &owned->owned_rows;
		int rc = owned->owned_rows
				 ? cli_application_present_screen(ctx, &ops, owned, cli_hooks_close)
				 : NB_ERR_NOMEM;

		if (rc)
			cli_hooks_close(owned, rc);
		return rc;
	}
	int rc = morph_tui_screen_open(&terminal, stdin, stdout,
				       MORPH_TUI_TERMINAL_SESSION_ALTERNATE |
					       MORPH_TUI_TERMINAL_SESSION_PASTE);

	if (rc == 0)
		rc = morph_tui_screen_run(&terminal, &ops, &controller);
	nb_buf_cleanup(&screen->frame);
	int leave_rc = morph_tui_screen_close(&terminal);

	if (rc == 0)
		rc = leave_rc;
	return rc;
}

int cli_hooks_ui(struct cli_application *ctx, cJSON **rows, const char *review_id)
{
	return cli_hooks_present(ctx, rows, review_id, 0);
}

int cli_hooks_review_all(struct cli_application *ctx, cJSON **rows)
{
	return cli_hooks_present(ctx, rows, NULL, 1);
}

int cli_hooks_startup(struct cli_application *ctx)
{
	cJSON *rows = NULL;
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
			rc = cli_hooks_review_all(ctx, &rows);
			break;
		}
	}
	cJSON_Delete(rows);
	return rc;
}
