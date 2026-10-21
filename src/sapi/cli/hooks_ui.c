#include "sapi/cli/hooks_ui.h"
#include "sapi/cli/list_ui.h"
#include "sapi/cli/setup_ui.h"
#include "runtime/runtime.h"
#include "util/arena.h"
#include "util/array.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/utf8.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define UI_TEXT "\033[0;38;2;225;229;234;48;2;16;18;20m"
#define UI_DIM "\033[0;38;2;142;150;161;48;2;16;18;20m"
#define UI_CYAN "\033[0;38;2;71;211;230;48;2;16;18;20m"
#define UI_GREEN "\033[0;38;2;124;217;167;48;2;16;18;20m"
#define UI_AMBER "\033[0;38;2;226;185;91;48;2;16;18;20m"
#define UI_RED "\033[0;38;2;240;163;163;48;2;49;24;26m"
#define UI_ADD "\033[0;38;2;160;227;182;48;2;22;46;33m"
#define UI_SELECTED "\033[0;38;2;239;244;248;48;2;29;48;56m"
#define UI_BAR "\033[0;38;2;71;211;230;48;2;29;48;56m"
#define UI_RULE "\033[0;38;2;48;57;65;48;2;16;18;20m"
#define KEY_UP 256
#define KEY_DOWN 257
#define KEY_LEFT 258
#define KEY_RIGHT 259
#define KEY_PAGE_UP 260
#define KEY_PAGE_DOWN 261
#define KEY_HOME 262
#define KEY_END 263

struct hook_line {
	const char *text;
	const char *style;
};

struct hook_screen {
	int height;
	int columns;
	int width;
	int focus;
	int review;
	int batch;
	int scroll;
	int choice;
	int page_size;
	int document_height;
	int document_lines;
	char message[256];
};

struct hook_action {
	const char *name;
	const char *label;
};

static const char *field(const cJSON *row, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(row, key);

	return cJSON_IsString(value) ? value->valuestring : "";
}

static const char *state_label(const cJSON *row)
{
	const char *state = field(row, "state");

	if (strcmp(state, "trusted") == 0) return "Trusted";
	if (strcmp(state, "needs_review") == 0) return "Needs review";
	if (strcmp(state, "changed / needs_review") == 0) return "Changed / needs review";
	if (strcmp(state, "project_untrusted") == 0) return "Workspace untrusted";
	if (strcmp(state, "disabled") == 0) return "Disabled";
	if (strcmp(state, "off") == 0) return "Hooks off";
	if (strcmp(state, "invalid") == 0) return "Invalid";
	if (strcmp(state, "unsupported") == 0) return "Unsupported";
	return state;
}

static const char *state_style(const cJSON *row)
{
	const char *state = field(row, "state");

	if (strcmp(state, "trusted") == 0) return UI_GREEN;
	if (strcmp(state, "disabled") == 0 || strcmp(state, "off") == 0) return UI_DIM;
	return UI_AMBER;
}

static const char *selected_state_style(const cJSON *row)
{
	const char *state = field(row, "state");

	if (strcmp(state, "trusted") == 0)
		return UI_GREEN "\033[48;2;29;48;56m";
	if (strcmp(state, "disabled") == 0 || strcmp(state, "off") == 0)
		return UI_DIM "\033[48;2;29;48;56m";
	return UI_AMBER "\033[48;2;29;48;56m";
}

/* Only trusted positioning/styles bypass the CLI output filter. */
static void cell(const struct hook_screen *screen, int row, int column, int width,
		 const char *text, const char *style)
{
	char clipped[BUFSIZ];

	if (row < 1 || row > screen->height || column > screen->columns || width <= 0)
		return;
	if (width > screen->columns - column)
		width = screen->columns - column;
	if (width <= 0)
		return;
	char *safe = utf8_terminal_sanitize_dup(text, strlen(text),
		UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe) return;
	utf8_copy_ellipsized_display_width(clipped, sizeof(clipped), safe,
					 (size_t)width, 0);
	free(safe);
	fprintf(stdout, "\033[%d;%dH%s%s%*s", row, column, style, clipped,
		width - (int)utf8_display_width(clipped), "");
}

static void rule(const struct hook_screen *screen, int row)
{
	if (row < 1 || row > screen->height)
		return;
	fprintf(stdout, "\033[%d;3H%s", row, UI_RULE);
	for (int i = 0; i < screen->width; i++)
		fprintf(stdout, "─");
}

static void selected(const struct hook_screen *screen, int row, const char *text)
{
	cell(screen, row, 3, screen->width, text, UI_SELECTED "\033[1m");
	cell(screen, row, 2, 1, "▏", UI_BAR);
}

static int actions(const cJSON *row, struct hook_action items[3])
{
	const char *state = field(row, "state");
	int count = 0;

	if (strcmp(state, "trusted") == 0) {
		items[count++] = (struct hook_action){"revoke", "Revoke trust"};
		items[count++] = (struct hook_action){"disable", "Disable hook"};
	} else if (strcmp(state, "disabled") == 0) {
		items[count++] = (struct hook_action){"enable", "Enable hook"};
		items[count++] = (struct hook_action){"revoke", "Revoke trust"};
	} else if (strcmp(state, "project_untrusted") == 0) {
		items[count++] = (struct hook_action){"trust-project", "Trust this workspace"};
	} else if (strcmp(state, "needs_review") == 0 ||
		   strcmp(state, "changed / needs_review") == 0) {
		items[count++] = (struct hook_action){"trust", "Trust current definition"};
	}
	items[count++] = (struct hook_action){NULL,
		strcmp(state, "trusted") == 0 ? "Back to hooks" : "Keep unchanged / Back to hooks"};
	return count;
}

static int add_line(struct arena *arena, morph_array_t *lines, const char *text,
		    const char *style, int width)
{
	char *safe = utf8_terminal_sanitize_dup(text, strlen(text),
		UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);

	if (!safe)
		MORPH_RETURN(-ENOMEM);
	const char *cursor = safe;
	int rc = 0;

	do {
		const char *end = utf8_advance_display_width(cursor, (size_t)width);
		if (*cursor && end == cursor)
			end = utf8_advance_codepoints(cursor, 1);
		size_t length = (size_t)(end - cursor);
		char *copy = arena_alloc(arena, length + 1);
		struct hook_line *line = morph_array_push(lines);

		if (!copy || !line) {
			MORPH_SET_ERR(rc, -ENOMEM);
			break;
		}
		memcpy(copy, cursor, length);
		copy[length] = '\0';
		*line = (struct hook_line){copy, style};
		cursor = end;
	} while (*cursor);
	free(safe);
	return rc;
}

static int detail_field(struct arena *arena, morph_array_t *lines, morph_buf_t *buffer,
			const char *label, const char *value, int width)
{
	morph_buf_reset(buffer);
	int rc = morph_buf_printf(buffer, "%-13s %s", label, *value ? value : "—");

	return rc == 0 ? add_line(arena, lines, morph_buf_cstr(buffer), UI_TEXT, width) : rc;
}

static int diff_property(struct arena *arena, morph_array_t *lines, morph_buf_t *buffer,
			 const cJSON *value, const char *prefix, const char *style, int width)
{
	char *json = cJSON_PrintUnformatted(value);
	if (!json)
		MORPH_RETURN(-ENOMEM);
	morph_buf_reset(buffer);
	int rc = morph_buf_printf(buffer, "%s  \"%s\": %s%s", prefix, value->string,
				 json, value->next ? "," : "");
	free(json);
	return rc == 0 ? add_line(arena, lines, morph_buf_cstr(buffer), style, width) : rc;
}

static int definition_lines(struct arena *arena, morph_array_t *lines, morph_buf_t *buffer,
			    const cJSON *row, int width)
{
	cJSON *current = cJSON_Parse(field(row, "definition"));
	cJSON *previous = cJSON_Parse(field(row, "previous_definition"));
	const cJSON *value;
	if (previous && cJSON_Compare(current, previous, 1)) {
		cJSON_Delete(previous);
		previous = NULL;
	}
	if (!cJSON_IsObject(current)) {
		cJSON_Delete(current);
		cJSON_Delete(previous);
		MORPH_RETURN(MORPH_ERR_PARSE);
	}
	int rc = add_line(arena, lines, previous ? "Definition changes" : "Current definition",
			  UI_TEXT "\033[1m", width);

	if (rc == 0) rc = add_line(arena, lines, "{", UI_DIM, width);
	cJSON_ArrayForEach(value, previous) {
		if (rc != 0) break;
		if (!cJSON_HasObjectItem(current, value->string))
			rc = diff_property(arena, lines, buffer, value, "-", UI_RED, width);
	}
	cJSON_ArrayForEach(value, current) {
		if (rc != 0) break;
		const cJSON *old = cJSON_GetObjectItemCaseSensitive(previous, value->string);
		int changed = previous && !cJSON_Compare(old, value, 1);

		if (changed && old)
			rc = diff_property(arena, lines, buffer, old, "-", UI_RED, width);
		if (rc == 0)
			rc = diff_property(arena, lines, buffer, value, changed ? "+" : " ",
					   changed ? UI_ADD : UI_TEXT, width);
	}
	if (rc == 0) rc = add_line(arena, lines, "}", UI_DIM, width);
	cJSON_Delete(current);
	cJSON_Delete(previous);
	return rc;
}

static int document(struct arena *arena, morph_array_t *lines, const cJSON *row,
		    int width, int review)
{
	morph_buf_t buffer;
	int rc = morph_buf_init_arena(&buffer, arena, 256);
	const char *keys[] = {"id", "command", "matcher", "source", "cwd"};
	const char *labels[] = {"ID", "Command", "Matcher", "Source", "Working dir"};

	if (rc == 0 && !review) {
		rc = morph_buf_printf(&buffer, "%s · %s", field(row, "event"), state_label(row));
		if (rc == 0)
			rc = add_line(arena, lines, morph_buf_cstr(&buffer), UI_TEXT "\033[1m", width);
	}

	for (size_t i = 0; rc == 0 && i < sizeof(keys) / sizeof(keys[0]); i++)
		rc = detail_field(arena, lines, &buffer, labels[i], field(row, keys[i]), width);
	if (rc == 0) {
		const cJSON *timeout = cJSON_GetObjectItemCaseSensitive(row, "timeout");
		char number[32];

		snprintf(number, sizeof(number), "%ds", cJSON_IsNumber(timeout) ? timeout->valueint : 0);
		rc = detail_field(arena, lines, &buffer, "Timeout", number, width);
	}
	if (rc == 0 && review) rc = add_line(arena, lines, "", UI_DIM, width);
	if (rc == 0 && review) rc = definition_lines(arena, lines, &buffer, row, width);
	if (rc == 0) rc = add_line(arena, lines, "", UI_DIM, width);
	if (rc == 0) rc = add_line(arena, lines, "Last execution", UI_TEXT "\033[1m", width);
	const cJSON *last = cJSON_GetObjectItemCaseSensitive(row, "last_run");
	if (rc == 0 && last) {
		morph_buf_reset(&buffer);
		rc = morph_buf_puts(&buffer, field(last, "name"));
		const cJSON *exit = cJSON_GetObjectItemCaseSensitive(last, "exit_code");
		const cJSON *duration = cJSON_GetObjectItemCaseSensitive(last, "duration_ms");

		if (rc == 0 && cJSON_IsNumber(exit))
			rc = morph_buf_printf(&buffer, " · exit %d", exit->valueint);
		if (rc == 0 && cJSON_IsNumber(duration))
			rc = morph_buf_printf(&buffer, " · %.0f ms", duration->valuedouble);
		if (rc == 0) rc = add_line(arena, lines, morph_buf_cstr(&buffer), UI_DIM, width);
		if (rc == 0 && *field(last, "error"))
			rc = detail_field(arena, lines, &buffer, "Error", field(last, "error"), width);
		if (rc == 0 && *field(last, "stderr"))
			rc = detail_field(arena, lines, &buffer, "Stderr", field(last, "stderr"), width);
	} else if (rc == 0) {
		rc = add_line(arena, lines, "Not run", UI_DIM, width);
	}
	if (rc == 0 && review)
		rc = add_line(arena, lines,
			"Trust covers this definition, not mutable script contents.", UI_DIM, width);
	if (rc == 0 && strcmp(field(row, "state"), "project_untrusted") == 0)
		rc = add_line(arena, lines,
			"Review this workspace path before trusting it; then review the hook definition.",
			UI_AMBER, width);
	return rc;
}

static void list_frame(struct hook_screen *screen, const cJSON *rows)
{
	int count = cJSON_GetArraySize(rows);
	int trusted = 0, disabled = 0, review = 0;
	const cJSON *row;
	char label[128];

	cJSON_ArrayForEach(row, rows) {
		trusted += strcmp(field(row, "state"), "trusted") == 0;
		disabled += strcmp(field(row, "state"), "disabled") == 0;
		review += strcmp(field(row, "state"), "needs_review") == 0 ||
			strcmp(field(row, "state"), "changed / needs_review") == 0 ||
			strcmp(field(row, "state"), "project_untrusted") == 0;
	}
	cell(screen, 3, 3, screen->width, "Lifecycle hooks", UI_TEXT "\033[1m");
	snprintf(label, sizeof(label), "%d trusted · %d need review · %d disabled",
		 trusted, review, disabled);
	if (count > trusted + disabled + review)
		snprintf(label, sizeof(label), "%d trusted · %d need review · %d disabled · %d unavailable",
			 trusted, review, disabled, count - trusted - disabled - review);
	cell(screen, 4, 3, screen->width, label, UI_DIM);
	rule(screen, 5);
	int state_width = screen->width >= 68 ? 22 : screen->width >= 40 ? 16 : 0;
	int source_width = screen->width >= 90 ? 10 : 0;
	int event_width = screen->width >= 68 ? 22 : screen->width >= 40 ? 20 : screen->width;
	int command_column = 3 + event_width;
	int state_column = 3 + screen->width - state_width;
	int source_column = state_column - source_width;
	int command_width = source_column - command_column - 2;

	cell(screen, 6, 3, event_width, "EVENT", UI_DIM);
	if (command_width > 4) cell(screen, 6, command_column, command_width, "COMMAND", UI_DIM);
	if (source_width) cell(screen, 6, source_column, source_width, "SOURCE", UI_DIM);
	if (state_width) cell(screen, 6, state_column, state_width, "STATE", UI_DIM);
	int start = screen->focus / screen->page_size * screen->page_size;
	int end = count - start > screen->page_size ? start + screen->page_size : count;

	for (int i = start; i < end; i++) {
		row = cJSON_GetArrayItem(rows, i);
		int y = 7 + i - start;
		int focus = i == screen->focus;
		const char *style = focus ? UI_SELECTED "\033[1m" : UI_TEXT;

		if (focus) selected(screen, y, "");
		cell(screen, y, 3, event_width, field(row, "event"), style);
		if (command_width > 4)
			cell(screen, y, command_column, command_width, field(row, "command"), style);
		if (source_width) {
			const char *source = field(row, "source");
			const char *cwd = field(row, "cwd");
			size_t length = strlen(cwd);
			int project = length > 0 && strncmp(source, cwd, length) == 0 &&
				strncmp(source + length, "/.morph/", 8) == 0;

			cell(screen, y, source_column, source_width, project ? "Project" : "User", style);
		}
		if (state_width) {
			cell(screen, y, state_column, state_width, state_label(row),
			     focus ? selected_state_style(row) : state_style(row));
		}
	}
	int summary = 7 + end - start;
	snprintf(label, sizeof(label), "Showing %d–%d of %d", count ? start + 1 : 0, end, count);
	cell(screen, summary, 3, screen->width, label, UI_DIM);
	if (!count) cell(screen, 7, 3, screen->width, "No hooks configured.", UI_DIM);
}

static void render_batch(struct hook_screen *screen, const cJSON *rows)
{
	int count = cJSON_GetArraySize(rows);
	int visible = (screen->height - 16) / 3;
	const char *choices[] = {
		"Trust all current hooks and remember",
		"Reject all current hooks and remember",
		"Review individually / keep unchanged"
	};
	char label[128];

	if (visible < 1)
		visible = 1;
	if (screen->scroll > count - visible)
		screen->scroll = count > visible ? count - visible : 0;
	screen->page_size = visible;
	cell(screen, 3, 3, screen->width, "Trust lifecycle hooks?", UI_TEXT "\033[1m");
	cell(screen, 4, 3, screen->width, field(cJSON_GetArrayItem(rows, 0), "cwd"), UI_DIM);
	cell(screen, 5, 3, screen->width,
		"User hooks: all workspaces. Project hooks: this workspace only.", UI_DIM);
	rule(screen, 6);
	for (int i = 0; i < visible && screen->scroll + i < count; i++) {
		const cJSON *row = cJSON_GetArrayItem(rows, screen->scroll + i);
		int y = 7 + i * 3;

		snprintf(label, sizeof(label), "%s · %s", field(row, "event"), state_label(row));
		cell(screen, y, 3, screen->width, label, state_style(row));
		cell(screen, y + 1, 5, screen->width - 2, field(row, "source"), UI_DIM);
		cell(screen, y + 2, 5, screen->width - 2, field(row, "command"), UI_TEXT);
	}
	snprintf(label, sizeof(label), "Showing %d–%d of %d · PgUp/PgDn to scroll",
		count ? screen->scroll + 1 : 0,
		screen->scroll + visible < count ? screen->scroll + visible : count, count);
	cell(screen, screen->height - 8, 3, screen->width, label, UI_DIM);
	rule(screen, screen->height - 7);
	for (int i = 0; i < 3; i++) {
		if (i == screen->choice)
			selected(screen, screen->height - 6 + i, choices[i]);
		else
			cell(screen, screen->height - 6 + i, 3, screen->width, choices[i], UI_TEXT);
	}
	cell(screen, screen->height - 2, 3, screen->width, screen->message, UI_AMBER);
	cell(screen, screen->height, 3, screen->width,
		"↑↓ Choose  Enter Confirm  Esc Keep unchanged", UI_DIM);
}

static int render(struct hook_screen *screen, const cJSON *rows)
{
	const cJSON *row = cJSON_GetArrayItem(rows, screen->focus);
	int height = screen->height;
	int detail_start;
	struct hook_action items[3];
	int action_count = actions(row, items);
	struct arena *arena = arena_create(BUFSIZ);
	morph_array_t lines = {0};

	if (!arena) MORPH_RETURN(-ENOMEM);
	struct hook_line element;
	int rc = morph_array_init(&lines, 32, sizeof(element));
	if (rc != 0) {
		arena_destroy(arena);
		return rc;
	}

	fprintf(stdout, "\033[?2026h%s\033[H\033[J", UI_TEXT);
	cell(screen, 1, 3, 5, "morph", UI_CYAN);
	cell(screen, 1, 9, screen->width - 6,
	     screen->review ? "/ hooks / review" : "/ hooks", UI_DIM);
	if (height < (screen->batch ? 20 : 12) ||
	    screen->width < (screen->batch ? 40 : 24)) {
		cell(screen, 3, 3, screen->width, "Resize terminal to view hooks.", UI_AMBER);
		cell(screen, height, 3, screen->width, "Esc Back / q Close", UI_DIM);
		screen->document_height = 0;
	} else if (screen->batch) {
		render_batch(screen, rows);
	} else if (cJSON_GetArraySize(rows) == 0) {
		screen->page_size = 1;
		screen->document_height = 0;
		screen->document_lines = 0;
		cell(screen, 3, 3, screen->width, "Lifecycle hooks", UI_TEXT "\033[1m");
		cell(screen, 6, 3, screen->width, "No hooks configured.", UI_TEXT);
		if (height >= 14) {
			cell(screen, 8, 3, screen->width, "Add hook definitions to:", UI_DIM);
			cell(screen, 9, 3, screen->width, "~/.morph/hooks.json   (user)", UI_DIM);
			cell(screen, 10, 3, screen->width, ".morph/hooks.json     (project)", UI_DIM);
		}
		cell(screen, height, 3, screen->width, "r Refresh  Esc Close", UI_DIM);
	} else {
		if (screen->review) {
			cell(screen, 3, 3, screen->width, field(row, "event"), UI_TEXT "\033[1m");
			cell(screen, 4, 3, screen->width, state_label(row), state_style(row));
			rule(screen, 5);
			detail_start = 6;
		} else {
			screen->page_size = height >= 24 ? (height - 20) / 2 : height - 10;
			if (screen->page_size < 1) screen->page_size = 1;
			int total = cJSON_GetArraySize(rows);
			if (total > 0 && screen->page_size > total) screen->page_size = total;
			list_frame(screen, rows);
			int start = screen->focus / screen->page_size * screen->page_size;
			int visible = total - start < screen->page_size ? total - start : screen->page_size;
			detail_start = 9 + visible;
			if (height < 24) detail_start = height - 2;
			else rule(screen, detail_start - 1);
		}
		int bottom = screen->review ? height - action_count - 5 : height - 3;
		screen->document_height = bottom - detail_start + 1;
		if (screen->document_height < 0) screen->document_height = 0;
		if (rc == 0 && row) rc = document(arena, &lines, row, screen->width, screen->review);
		screen->document_lines = lines.nelts > INT_MAX ? INT_MAX : (int)lines.nelts;
		int max_scroll = screen->document_lines - screen->document_height;
		if (max_scroll < 0) max_scroll = 0;
		if (screen->scroll > max_scroll) screen->scroll = max_scroll;
		for (int i = 0; rc == 0 && i < screen->document_height; i++) {
			struct hook_line *line = morph_array_get(&lines, (size_t)(screen->scroll + i));
			if (!line) break;
			cell(screen, detail_start + i, 3, screen->width, line->text, line->style);
		}
		if (screen->review) {
			int title = height - action_count - 3;

			rule(screen, title - 1);
			cell(screen, title, 3, screen->width, "Review decision", UI_TEXT "\033[1m");
			for (int i = 0; i < action_count; i++) {
				if (i == screen->choice) selected(screen, title + 1 + i, items[i].label);
				else cell(screen, title + 1 + i, 3, screen->width, items[i].label, UI_TEXT);
			}
		}
		cell(screen, height, 3, screen->width, screen->review ?
		     "↑↓ Choose  Enter Confirm  PgUp/PgDn Scroll  Esc Back" :
		     cJSON_GetArraySize(rows) > screen->page_size ?
		     "↑↓ Select  Enter Review  a Review all  ←→ Page  r Refresh  Esc Close" :
		     "↑↓ Select  Enter Review  a Review all  r Refresh  Esc Close", UI_DIM);
	}
	if (!screen->batch && *screen->message)
		cell(screen, height - 1, 3, screen->width, screen->message, UI_AMBER);
	else if (screen->review && screen->document_height > 0 &&
		 screen->document_lines > screen->document_height) {
		char status[80];

		snprintf(status, sizeof(status), "Lines %d–%d / %d · PgUp/PgDn to read more",
			 screen->scroll + 1, screen->scroll + screen->document_height,
			 screen->document_lines);
		cell(screen, height - 1, 3, screen->width, status, UI_DIM);
	}
	fprintf(stdout, "\033[0m\033[?2026l");
	fflush(stdout);
	morph_array_cleanup(&lines);
	arena_destroy(arena);
	return rc;
}

static int read_byte(int timeout)
{
	struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
	unsigned char byte;
	int rc = poll(&input, 1, timeout);

	if (rc < 0) MORPH_RETURN(-errno);
	if (!rc) return 0;
	ssize_t size = read(STDIN_FILENO, &byte, 1);
	if (size < 0) MORPH_RETURN(-errno);
	return size ? byte : -ECANCELED;
}

static int read_key(void)
{
	int key = read_byte(100);

	if (key != 27) return key;
	key = read_byte(80);
	if (!key) return 27;
	if (key < 0) return key;
	if (key != '[' && key != 'O') return 0;
	char sequence[16];
	size_t length = 0;

	while (length + 1 < sizeof(sequence)) {
		key = read_byte(80);
		if (key <= 0) return key;
		sequence[length++] = (char)key;
		if (key >= '@' && key <= '~') break;
	}
	sequence[length] = '\0';
	if (strcmp(sequence, "A") == 0) return KEY_UP;
	if (strcmp(sequence, "B") == 0) return KEY_DOWN;
	if (strcmp(sequence, "C") == 0) return KEY_RIGHT;
	if (strcmp(sequence, "D") == 0) return KEY_LEFT;
	if (strcmp(sequence, "5~") == 0) return KEY_PAGE_UP;
	if (strcmp(sequence, "6~") == 0) return KEY_PAGE_DOWN;
	if (strcmp(sequence, "H") == 0 || strcmp(sequence, "1~") == 0) return KEY_HOME;
	if (strcmp(sequence, "F") == 0 || strcmp(sequence, "4~") == 0) return KEY_END;
	return 0;
}

static int refresh(struct cli_context *ctx, cJSON **rows, struct hook_screen *screen)
{
	cJSON *fresh = NULL;
	const cJSON *row = cJSON_GetArrayItem(*rows, screen->focus);
	char id[65];

	snprintf(id, sizeof(id), "%s", field(row, "id"));
	int rc = runtime_hooks_snapshot(ctx->runtime, &fresh);
	if (rc != 0) return rc;
	cJSON_Delete(*rows);
	*rows = fresh;
	screen->focus = 0;
	cJSON_ArrayForEach(row, fresh) {
		if (strcmp(field(row, "id"), id) == 0) break;
		screen->focus++;
	}
	if (screen->focus >= cJSON_GetArraySize(fresh)) {
		screen->focus = 0;
		screen->review = 0;
	}
	struct hook_action items[3];
	screen->choice = actions(cJSON_GetArrayItem(fresh, screen->focus), items) - 1;
	screen->scroll = 0;
	return 0;
}

static int hooks_ui(struct cli_context *ctx, cJSON **rows, const char *review_id, int batch)
{
	struct hook_screen screen = {.batch = batch, .choice = batch ? 2 : 0, .page_size = 1};
	struct termios saved;
	const cJSON *row;
	int rc = 0;
	int dirty = 1;

	if (review_id) {
		int found = 0;
		cJSON_ArrayForEach(row, *rows) {
			if (strcmp(field(row, "id"), review_id) == 0) { found = 1; break; }
			screen.focus++;
		}
		if (!found) MORPH_RETURN(-ENOENT);
		screen.review = 1;
		struct hook_action items[3];
		screen.choice = actions(row, items) - 1;
	}
	if (tcgetattr(STDIN_FILENO, &saved) != 0) MORPH_RETURN(-errno);
	struct termios raw = saved;
	raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IEXTEN);
	raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) MORPH_RETURN(-errno);
	fprintf(stdout, "\033[?1049h\033[?25l");
	for (;;) {
		struct winsize ws;
		int height = 24, columns = 80;

		if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
			if (ws.ws_row) height = ws.ws_row;
			if (ws.ws_col) columns = ws.ws_col;
		}
		if (dirty || height != screen.height || columns != screen.columns) {
			screen.height = height;
			screen.columns = columns;
			screen.width = columns > 4 ? columns - 4 : 1;
			rc = render(&screen, *rows);
			if (rc != 0) break;
			dirty = 0;
		}
		int key = read_key();
		if (!key || key == -EINTR) continue;
		if (key < 0) {
			if (key != -ECANCELED) MORPH_SET_ERR(rc, key);
			break;
		}
		if (key == 3 || key == 4 || key == 'q') break;
		if (key == 27) {
			if (!screen.review) break;
			screen.review = 0;
			screen.scroll = 0;
			screen.message[0] = '\0';
			dirty = 1;
			continue;
		}
		if (key == 'r' && !screen.review) {
			rc = refresh(ctx, rows, &screen);
			if (rc != 0) break;
			screen.message[0] = '\0';
			dirty = 1;
			continue;
		}
		if (screen.height < (screen.batch ? 20 : 12) ||
		    screen.width < (screen.batch ? 40 : 24) ||
		    (screen.review && screen.document_height == 0)) continue;
		if (screen.batch) {
			if (key == KEY_UP || key == 'k') screen.choice = (screen.choice + 2) % 3;
			if (key == KEY_DOWN || key == 'j' || key == '\t')
				screen.choice = (screen.choice + 1) % 3;
			if (key == KEY_PAGE_DOWN) screen.scroll += screen.page_size;
			if (key == KEY_PAGE_UP) screen.scroll -= screen.page_size;
			if (screen.scroll < 0) screen.scroll = 0;
			if (key == '\r' || key == '\n') {
				if (screen.choice == 2) {
					screen.batch = 0;
					screen.scroll = 0;
				} else {
					row = cJSON_GetArrayItem(*rows, 0);
					int result = runtime_hooks_manage(ctx->runtime,
						screen.choice == 0 ? "trust-all" : "deny-all",
						"workspace", field(row, "review_digest"));
					if (result == 0) break;
					rc = refresh(ctx, rows, &screen);
					if (rc != 0) break;
					screen.choice = 2;
					snprintf(screen.message, sizeof(screen.message),
						"Action failed: %s. Review the refreshed hooks.",
						morph_strerror(result));
				}
			}
			dirty = 1;
			continue;
		}
		if (key == 'a' && !screen.review && cJSON_GetArraySize(*rows) > 0) {
			rc = refresh(ctx, rows, &screen);
			if (rc != 0) break;
			screen.batch = 1;
			screen.choice = 2;
			dirty = 1;
			continue;
		}
		row = cJSON_GetArrayItem(*rows, screen.focus);
		struct hook_action items[3];
		int count = actions(row, items);
		int total = cJSON_GetArraySize(*rows);

		if (screen.review) {
			if (key == KEY_UP || key == 'k') screen.choice = (screen.choice + count - 1) % count;
			if (key == KEY_DOWN || key == 'j' || key == '\t') screen.choice = (screen.choice + 1) % count;
			if (key == KEY_PAGE_UP) screen.scroll -= screen.document_height;
			if (key == KEY_PAGE_DOWN) screen.scroll += screen.document_height;
			if (key == KEY_HOME) screen.scroll = 0;
			if (key == KEY_END) screen.scroll = screen.document_lines;
			if (screen.scroll < 0) screen.scroll = 0;
		}
		if (key == '\r' || key == '\n') {
			if (!row) continue;
			if (!screen.review) {
				rc = refresh(ctx, rows, &screen);
				if (rc != 0) break;
				screen.review = cJSON_GetArraySize(*rows) > 0;
			} else if (!items[screen.choice].name) {
				screen.review = 0;
				screen.scroll = 0;
			} else {
				const char *action = items[screen.choice].name;
				int project = strcmp(action, "trust-project") == 0;
				int result = runtime_hooks_manage(ctx->runtime, action,
					project ? "workspace" : field(row, "id"),
					field(row, project ? "workspace_digest" : "digest"));
				snprintf(screen.message, sizeof(screen.message), result == 0 ?
					"Completed: %s" : "Action failed: %s", result == 0 ? action : morph_strerror(result));
				rc = refresh(ctx, rows, &screen);
				if (rc != 0) break;
			}
		} else if (!screen.review && total > 0) {
			if ((key == KEY_UP || key == 'k') && screen.focus > 0) screen.focus--;
			if ((key == KEY_DOWN || key == 'j') && screen.focus < total - 1) screen.focus++;
			if (key == KEY_RIGHT || key == KEY_PAGE_DOWN)
				screen.focus += screen.page_size < total - 1 - screen.focus ?
					screen.page_size : total - 1 - screen.focus;
			if (key == KEY_LEFT || key == KEY_PAGE_UP)
				screen.focus = screen.focus >= screen.page_size ? screen.focus - screen.page_size : 0;
			if (key == KEY_HOME) screen.focus = 0;
			if (key == KEY_END && total) screen.focus = total - 1;
			screen.scroll = 0;
		}
		dirty = 1;
	}
	fprintf(stdout, "\033[0m\033[?25h\033[?1049l");
	fflush(stdout);
	if (tcsetattr(STDIN_FILENO, TCSANOW, &saved) != 0 && rc == 0)
		MORPH_SET_ERR(rc, -errno);
	return rc;
}

int cli_hooks_ui(struct cli_context *ctx, cJSON **rows, const char *review_id)
{
	return hooks_ui(ctx, rows, review_id, 0);
}

int cli_hooks_review_all(struct cli_context *ctx, cJSON **rows)
{
	return hooks_ui(ctx, rows, NULL, 1);
}

int cli_hooks_startup(struct cli_context *ctx)
{
	cJSON *rows = NULL;
	const cJSON *row;
	int rc;

	if (!cli_setup_ui_available(stdin, stdout))
		return 0;
	rc = runtime_hooks_snapshot(ctx->runtime, &rows);
	if (rc != 0)
		return rc;
	cJSON_ArrayForEach(row, rows) {
		const char *state = field(row, "state");

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
