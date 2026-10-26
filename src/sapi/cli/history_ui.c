#include "sapi/cli/internal.h"
#include "sapi/cli/history_ui.h"
#include "sapi/cli/list_ui.h"

#include <sys/ioctl.h>
#include <termios.h>

#define HISTORY_QUERY_MAX 256
#define HISTORY_PREVIEW_MAX 240
#define HISTORY_UP 256
#define HISTORY_DOWN 257
#define HISTORY_HOME 258
#define HISTORY_END 259
#define HISTORY_PAGE_UP 260
#define HISTORY_PAGE_DOWN 261
#define HISTORY_PASTE_BEGIN 262
#define HISTORY_PASTE_END 263

struct history_line {
	size_t offset;
	size_t length;
};

struct history_view {
	struct cli_context *ctx;
	cJSON *report;
	morph_array_t matches;
	morph_array_t lines;
	morph_buf_t document;
	morph_buf_t previous;
	morph_buf_t query;
	size_t focus;
	size_t scroll;
	int filter;
	int searching;
	int pasting;
	int detail;
	int stored;
	int rows;
	int columns;
	int previous_rows;
	int previous_columns;
	const char *message;
};

static volatile sig_atomic_t history_signal;

static void history_handle_signal(int signal_number)
{
	if (signal_number != SIGWINCH)
		history_signal = signal_number;
}

static const char *history_string(const cJSON *object, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);

	return cJSON_IsString(value) ? value->valuestring : "";
}

static int64_t history_number(const cJSON *object, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);

	return cJSON_IsNumber(value) ? (int64_t)value->valuedouble : 0;
}

static const char *history_label(const cJSON *entry)
{
	const char *kind = history_string(entry, "kind");

	if (strcmp(kind, "user_message") == 0) return "You";
	if (strcmp(kind, "assistant_message") == 0) return "Assistant";
	if (strcmp(kind, "assistant_tool_calls") == 0) return "Tool call";
	if (strcmp(kind, "tool_result") == 0) return "Tool result";
	if (strcmp(kind, "compaction_summary") == 0) return "Summary";
	if (strcmp(kind, "background_receipt") == 0) return "Background";
	return kind;
}

static int history_matches_filter(const cJSON *entry, int filter)
{
	const char *status = history_string(entry, "status");

	return filter == 0 || (filter == 1 && strncmp(status, "IN", 2) == 0) ||
		(filter == 2 && strcmp(status, "VIA SUMMARY") == 0) ||
		(filter == 3 && strcmp(status, "OUT") == 0);
}

static int history_contains(const char *text, const char *query)
{
	size_t length = strlen(query);

	if (!length)
		return 1;
	for (; *text; text++) {
		if (strncasecmp(text, query, length) == 0)
			return 1;
	}
	return 0;
}

static int history_filter(struct history_view *view)
{
	cJSON *entry;
	const char *query = morph_buf_cstr(&view->query);

	morph_array_clear(&view->matches);
	cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(view->report, "entries")) {
		char sequence[32];

		snprintf(sequence, sizeof(sequence), "#%lld",
			(long long)history_number(entry, "sequence_no"));
		if (!history_matches_filter(entry, view->filter))
			continue;
		if (!history_contains(history_string(entry, "content"), query) &&
		    !history_contains(history_string(entry, "payload_json"), query) &&
		    !history_contains(history_string(entry, "tool_name"), query) &&
		    !history_contains(history_string(entry, "tool_call_id"), query) &&
		    !history_contains(sequence, query) &&
		    !history_contains(history_label(entry), query))
			continue;
		cJSON **slot = morph_array_push(&view->matches);

		if (!slot)
			MORPH_RETURN(-ENOMEM);
		*slot = entry;
	}
	if (view->focus >= view->matches.nelts)
		view->focus = view->matches.nelts ? view->matches.nelts - 1 : 0;
	return 0;
}

static cJSON *history_selected(struct history_view *view)
{
	cJSON **slot = morph_array_get(&view->matches, view->focus);

	return slot ? *slot : NULL;
}

static void history_preview(const cJSON *entry, char *preview, size_t capacity)
{
	const char *content = history_string(entry, "content");
	cJSON *payload = NULL;
	morph_buf_t call = {0};
	char *limited;

	if (strcmp(history_string(entry, "kind"), "assistant_tool_calls") == 0) {
		payload = cJSON_Parse(history_string(entry, "payload_json"));
		cJSON *calls = cJSON_GetObjectItemCaseSensitive(payload, "calls");
		cJSON *first = cJSON_GetArrayItem(calls, 0);
		const char *arguments = history_string(first, "arguments");

		if (!*arguments)
			arguments = history_string(first, "input");
		char *short_arguments = utf8_dup_clamped(arguments, HISTORY_PREVIEW_MAX);

		if (first && short_arguments && morph_buf_init(&call, 128) == 0) {
			(void)morph_buf_printf(&call, "%s %s", history_string(first, "name"),
				short_arguments);
			if (cJSON_GetArraySize(calls) > 1)
				(void)morph_buf_printf(&call, " (+%d calls)", cJSON_GetArraySize(calls) - 1);
			content = morph_buf_cstr(&call);
		}
		free(short_arguments);
	}
	if (!*content)
		content = history_string(entry, "payload_json");
	limited = utf8_dup_clamped(content, HISTORY_PREVIEW_MAX);
	cli_list_compact_text(preview, capacity, limited ? limited : "");
	free(limited);
	morph_buf_cleanup(&call);
	cJSON_Delete(payload);
}

static int history_json_text(const cJSON *json, morph_buf_t *text)
{
	char *pretty = cJSON_Print(json);
	int rc;

	if (!pretty)
		MORPH_RETURN(-ENOMEM);
	rc = morph_buf_puts(text, pretty);
	free(pretty);
	return rc;
}

static int history_request_document(struct cli_context *ctx, morph_buf_t *text)
{
	cJSON *snapshot = NULL;
	int rc = runtime_session_last_request(ctx->runtime, &snapshot);

	if (rc == -ENODATA)
		return morph_buf_puts(text, "No captured request for this session.\n"
			"Snapshots are recorded for new structured model calls.\n");
	if (rc != 0)
		return rc;
	rc = morph_buf_printf(text, "Last submitted model input\nModel: %s / %s\n"
		"Turn: %s\nCaptured at: %lld (Unix time)\n\n"
		"Captured before dispatch; provider acceptance is not implied.\n"
		"Adapter-normalized input; credentials redacted, headers and generation options omitted.\n\n",
		history_string(snapshot, "provider"), history_string(snapshot, "model"),
		history_string(snapshot, "turn_id"),
		(long long)history_number(snapshot, "created_at"));
	cJSON *input = cJSON_Parse(history_string(snapshot, "input_json"));

	if (rc == 0)
		rc = input ? history_json_text(input, text) : MORPH_ERR_PARSE;
	cJSON_Delete(input);
	cJSON_Delete(snapshot);
	return rc;
}

static int history_entry_document(struct cli_context *ctx, const cJSON *entry,
	int stored, morph_buf_t *text)
{
	int64_t sequence = history_number(entry, "sequence_no");
	int rc = morph_buf_printf(text, "#%lld  %s  %s\nNext turn: %s · %lld stored tokens\n"
		"%s\nTurn: %s\nCall ID: %s\n\n%s\n\n",
		(long long)sequence, history_label(entry), history_string(entry, "tool_name"),
		history_string(entry, "status"), (long long)history_number(entry, "token_count"),
		history_string(entry, "reason"), history_string(entry, "turn_id"),
		history_string(entry, "tool_call_id"), stored ? "Stored view" : "Estimated request view");

	if (rc != 0)
		return rc;
	if (stored) {
		rc = morph_buf_puts(text, history_string(entry, "content"));
		const char *payload = history_string(entry, "payload_json");

		if (rc == 0 && *payload) {
			cJSON *json = cJSON_Parse(payload);

			rc = morph_buf_puts(text, "\n\nStored metadata\n");
			if (rc == 0)
				rc = json ? history_json_text(json, text) : morph_buf_puts(text, payload);
			cJSON_Delete(json);
		}
		return rc;
	}
	cJSON *input = NULL;

	rc = runtime_session_history_entry_input(ctx->runtime, sequence, &input);
	if (rc == -ENODATA || rc == -ENOENT)
		return morph_buf_puts(text, "Original entry is not directly sent in the current window.\n"
			"Use Stored view for retained content. VIA SUMMARY entries are represented by summaries.\n"
			"LEGACY entries require migration and validation before replay.\n");
	if (rc == 0)
		rc = history_json_text(input, text);
	cJSON_Delete(input);
	return rc;
}

static int history_document(struct history_view *view)
{
	morph_buf_t text = {0};
	int rc = morph_buf_init(&text, BUFSIZ);

	if (rc != 0)
		return rc;
	if (view->detail == 2)
		rc = history_request_document(view->ctx, &text);
	else if (history_selected(view))
		rc = history_entry_document(view->ctx, history_selected(view), view->stored, &text);
	char *safe = rc == 0 ? utf8_terminal_sanitize_dup(morph_buf_cstr(&text), text.len,
		UTF8_TERMINAL_TEXT_MULTILINE, NULL) : NULL;

	if (rc == 0 && !safe)
		rc = -ENOMEM;
	morph_buf_reset(&view->document);
	if (rc == 0) {
		for (char *p = safe; *p; p++) {
			if (*p == '\t')
				*p = ' ';
		}
		rc = morph_buf_puts(&view->document, safe);
	}
	free(safe);
	morph_buf_cleanup(&text);
	return rc;
}

static int history_wrap(struct history_view *view)
{
	const char *start = morph_buf_cstr(&view->document);
	int width = view->columns > 4 ? view->columns - 4 : 1;

	morph_array_clear(&view->lines);
	while (*start) {
		const char *newline = strchr(start, '\n');
		const char *end = utf8_advance_display_width(start, (size_t)width);

		if (newline && newline < end)
			end = newline;
		if (end == start && *start != '\n')
			end += utf8codepointcalcsize(start);
		struct history_line *line = morph_array_push(&view->lines);

		if (!line)
			MORPH_RETURN(-ENOMEM);
		line->offset = (size_t)(start - view->document.data);
		line->length = (size_t)(end - start);
		start = *end == '\n' ? end + 1 : end;
	}
	return 0;
}

static int history_frame_line(morph_buf_t *frame, int columns, const char *text,
	const char *style)
{
	char clipped[BUFSIZ];
	char *safe = utf8_terminal_sanitize_dup(text, strlen(text),
		UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	int rc;

	if (!safe)
		MORPH_RETURN(-ENOMEM);
	utf8_copy_ellipsized_display_width(clipped, sizeof(clipped), safe,
		(size_t)(columns > 1 ? columns - 1 : 0), 0);
	free(safe);
	rc = morph_buf_printf(frame, "%s%s%s\n", cli_color_enabled() ? style : "",
		clipped, cli_color_enabled() ? ANSI_RESET : "");
	return rc;
}

static int history_page_size(const struct history_view *view)
{
	int overhead = view->detail ? 5 : 11;

	return view->rows > overhead ? view->rows - overhead : 1;
}

static int history_render(struct history_view *view)
{
	morph_buf_t frame = {0};
	morph_buf_t text = {0};
	int rc = morph_buf_init(&frame, BUFSIZ);

	if (rc == 0)
		rc = morph_buf_init(&text, BUFSIZ);
	if (rc != 0) {
		morph_buf_cleanup(&frame);
		return rc;
	}
	int page = history_page_size(view);
	size_t first = view->focus >= (size_t)page ? view->focus - (size_t)page + 1 : 0;
	size_t max_scroll = view->lines.nelts > (size_t)page ? view->lines.nelts - (size_t)page : 0;

	if (view->scroll > max_scroll)
		view->scroll = max_scroll;
	for (int row = 1; row <= view->rows && rc == 0; row++) {
		const char *style = ANSI_DIM;

		morph_buf_reset(&text);
		if (view->columns < 32 || view->rows < 12) {
			if (row == 1)
				(void)morph_buf_puts(&text, "Widen terminal · Esc back");
		} else if (row == 1) {
			(void)morph_buf_puts(&text, view->detail == 2 ?
				"  morph / history / last submitted input" : view->detail ?
				"  morph / history / entry details" : "  morph / history");
			style = ANSI_BOLD;
		} else if (view->detail && row >= 3 && row < 3 + page) {
			struct history_line *line = morph_array_get(&view->lines,
				view->scroll + (size_t)(row - 3));

			if (line) {
				(void)morph_buf_puts(&text, "  ");
				(void)morph_buf_append(&text, view->document.data + line->offset, line->length);
				style = "";
			}
		} else if (view->detail && row == view->rows - 1) {
			(void)morph_buf_printf(&text, "  Lines %zu–%zu of %zu", view->scroll + 1,
				view->scroll + (size_t)page < view->lines.nelts ?
				view->scroll + (size_t)page : view->lines.nelts, view->lines.nelts);
		} else if (view->detail && row == view->rows) {
			(void)morph_buf_puts(&text, view->columns < 76 ?
				(view->detail == 2 ? "  Up/Down PgUp/PgDn · Esc back" :
				 "  v view · PgUp/PgDn · Esc back") : view->detail == 2 ?
				"  Up/Down PgUp/PgDn scroll · Esc back" :
				"  v request/stored · s last request · PgUp/PgDn scroll · Esc back");
		} else if (!view->detail && row == 2) {
			(void)morph_buf_printf(&text, "  %d saved entries · %zu matching",
				cJSON_GetArraySize(cJSON_GetObjectItem(view->report, "entries")),
				view->matches.nelts);
			if (view->columns >= 76)
				(void)morph_buf_printf(&text, " · %s", history_string(view->report, "model"));
		} else if (!view->detail && row == 4) {
			if (view->columns < 76)
				(void)morph_buf_printf(&text, "  Next (est): %lld items · %lld tok",
					(long long)history_number(view->report, "active_items"),
					(long long)history_number(view->report, "history_tokens"));
			else
				(void)morph_buf_printf(&text,
					"  Next turn · estimated history: %lld tokens / %lld limit",
					(long long)history_number(view->report, "history_tokens"),
					(long long)history_number(view->report, "context_limit"));
		} else if (!view->detail && row == 5) {
			(void)morph_buf_puts(&text, cJSON_IsTrue(cJSON_GetObjectItem(view->report, "needs_repair")) ?
				"  Replay needs repair before send; current membership may change." :
				view->columns < 76 ? "  History only; prompt/tools are extra." :
				"  History only; instructions, tools and new input are additional.");
		} else if (!view->detail && row == 6) {
			const char *filters[] = {"All", "Included", "Summarized", "Excluded"};

			if (view->columns < 50)
				(void)morph_buf_printf(&text, "  Filter: %s (Tab)", filters[view->filter]);
			else {
				for (int i = 0; i < 4; i++)
					(void)morph_buf_printf(&text, "  %s%s%s", i == view->filter ? "[" : "",
						filters[i], i == view->filter ? "]" : "");
			}
		} else if (!view->detail && row == 8) {
			(void)morph_buf_printf(&text, "  ENTRY / ACTIVITY%*sNEXT TURN",
				view->columns - 37, "");
		} else if (!view->detail && row >= 9 && row < 9 + page) {
			size_t index = first + (size_t)(row - 9);
			cJSON **slot = morph_array_get(&view->matches, index);

			if (slot) {
				char preview[HISTORY_PREVIEW_MAX + 1];
				char clipped[BUFSIZ];
				int left = view->columns - 20;
				cJSON *entry = *slot;

				history_preview(entry, preview, sizeof(preview));
				(void)morph_buf_printf(&text, "%s #%lld %s %s %s",
					index == view->focus ? ">" : " ",
					(long long)history_number(entry, "sequence_no"), history_label(entry),
					history_string(entry, "tool_name"), preview);
				char *safe = utf8_terminal_sanitize_dup(morph_buf_cstr(&text), text.len,
					UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);

				utf8_copy_ellipsized_display_width(clipped, sizeof(clipped),
					safe ? safe : "", (size_t)left, 0);
				free(safe);
				morph_buf_reset(&text);
				(void)morph_buf_printf(&text, "%s%*s%s", clipped,
					left - (int)utf8_display_width(clipped) + 1, "",
					history_string(entry, "status"));
				const char *status = history_string(entry, "status");

				style = index == view->focus ? ANSI_CYAN "\033[7m" :
					strcmp(status, "OUT") == 0 ? ANSI_DIM :
					strcmp(status, "IN") == 0 ? "" : ANSI_YELLOW;
			} else if (!view->matches.nelts && row == 9) {
				(void)morph_buf_puts(&text, "  No matching history entries.");
			}
		} else if (!view->detail && row == view->rows - 2) {
			(void)morph_buf_puts(&text, "  IN direct · VIA SUMMARY condensed · OUT not sent");
		} else if (!view->detail && row == view->rows - 1) {
			(void)morph_buf_printf(&text, "  %s%s%s", view->searching ? "Search: " : "/ ",
				morph_buf_cstr(&view->query), view->message ? view->message : "");
		} else if (!view->detail && row == view->rows) {
			(void)morph_buf_puts(&text, view->columns < 70 ?
				"  Up/Down Enter / Tab s Esc back" :
				"  Up/Down · Enter details · / search · Tab filter · s request · Esc back");
		}
		if (text.failed)
			rc = -ENOMEM;
		else
			rc = history_frame_line(&frame, view->columns, morph_buf_cstr(&text), style);
	}
	if (rc == 0) {
		const char *current = morph_buf_cstr(&frame);
		const char *previous = morph_buf_cstr(&view->previous);
		int resized = view->previous_rows != view->rows ||
			view->previous_columns != view->columns;

		fprintf(stdout, "\033[?2026h");
		if (resized)
			fprintf(stdout, "\033[H\033[2J");
		for (int row = 1; row <= view->rows; row++) {
			size_t length = strcspn(current, "\n");
			size_t old_length = strcspn(previous, "\n");

			if (resized || length != old_length || memcmp(current, previous, length) != 0)
				fprintf(stdout, "\033[%d;1H%.*s\033[0m\033[K", row, (int)length, current);
			current += length + (current[length] == '\n');
			previous += old_length + (previous[old_length] == '\n');
		}
		fprintf(stdout, "\033[?2026l");
		fflush(stdout);
		morph_buf_reset(&view->previous);
		rc = morph_buf_append(&view->previous, frame.data, frame.len);
		view->previous_rows = view->rows;
		view->previous_columns = view->columns;
	}
	morph_buf_cleanup(&text);
	morph_buf_cleanup(&frame);
	return rc;
}

static int history_read_byte(int timeout)
{
	struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
	unsigned char byte;
	int rc = poll(&input, 1, timeout);

	if (rc < 0)
		MORPH_RETURN(-errno);
	if (rc == 0)
		return 0;
	ssize_t count = read(STDIN_FILENO, &byte, 1);

	MORPH_RETURN(count == 1 ? byte : count == 0 ? -ECANCELED : -errno);
}

static int history_read_key(void)
{
	int key = history_read_byte(100);
	char sequence[32];
	size_t length = 0;

	if (key != 27)
		return key;
	key = history_read_byte(60);
	if (key == 0)
		return 27;
	if (key != '[' && key != 'O')
		return key < 0 ? key : 0;
	while (length + 1 < sizeof(sequence)) {
		key = history_read_byte(60);
		if (key <= 0)
			return key;
		sequence[length++] = (char)key;
		if (key >= 0x40 && key <= 0x7e)
			break;
	}
	sequence[length] = '\0';
	if (strcmp(sequence, "A") == 0) return HISTORY_UP;
	if (strcmp(sequence, "B") == 0) return HISTORY_DOWN;
	if (strcmp(sequence, "H") == 0 || strcmp(sequence, "1~") == 0) return HISTORY_HOME;
	if (strcmp(sequence, "F") == 0 || strcmp(sequence, "4~") == 0) return HISTORY_END;
	if (strcmp(sequence, "5~") == 0) return HISTORY_PAGE_UP;
	if (strcmp(sequence, "6~") == 0) return HISTORY_PAGE_DOWN;
	if (strcmp(sequence, "200~") == 0) return HISTORY_PASTE_BEGIN;
	if (strcmp(sequence, "201~") == 0) return HISTORY_PASTE_END;
	return 0;
}

int cli_history_view(struct cli_context *ctx, cJSON *report)
{
	struct history_view view = {.ctx = ctx, .report = report, .rows = 24, .columns = 80};
	cJSON *entry;
	struct history_line line;
	struct termios saved;
	struct sigaction action = {0};
	struct sigaction previous[4];
	const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGWINCH};
	int installed = 0;
	int entered = 0;
	int screen_active = 0;
	int dirty = 1;
	int caught = 0;
	int rc = morph_array_init(&view.matches, 32, sizeof(entry));

	if (rc == 0) rc = morph_array_init(&view.lines, 32, sizeof(line));
	if (rc == 0) rc = morph_buf_init(&view.document, BUFSIZ);
	if (rc == 0) rc = morph_buf_init(&view.previous, BUFSIZ);
	if (rc == 0) rc = morph_buf_init(&view.query, HISTORY_QUERY_MAX);
	if (rc == 0) rc = history_filter(&view);
	if (rc == 0 && view.matches.nelts)
		view.focus = view.matches.nelts - 1;
	if (rc == 0 && tcgetattr(STDIN_FILENO, &saved) != 0)
		rc = -errno;
	if (rc == 0) {
		struct termios raw = saved;

		raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IEXTEN);
		raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0;
		if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
			rc = -errno;
		else
			entered = 1;
	}
	history_signal = 0;
	action.sa_handler = history_handle_signal;
	sigemptyset(&action.sa_mask);
	while (rc == 0 && installed < 4) {
		if (sigaction(signals[installed], &action, &previous[installed]) != 0)
			rc = -errno;
		else
			installed++;
	}
	if (rc == 0) {
		fprintf(stdout, "\033[?1049h\033[?25l\033[?2004h");
		screen_active = 1;
	}
	while (rc == 0) {
		struct winsize size = {0};

		if (history_signal) {
			caught = history_signal;
			break;
		}
		if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col && size.ws_row &&
		    (view.rows != size.ws_row || view.columns != size.ws_col)) {
			view.rows = size.ws_row;
			view.columns = size.ws_col;
			if (view.detail)
				rc = history_wrap(&view);
			dirty = 1;
		}
		if (rc == 0 && dirty)
			rc = history_render(&view);
		if (rc != 0)
			break;
		dirty = 0;
		int key = history_read_key();

		if (key == -EINTR || key == 0)
			continue;
		if (key < 0) {
			rc = key == -ECANCELED ? 0 : key;
			break;
		}
		if (key == 3 || key == 4)
			break;
		if (key == HISTORY_PASTE_BEGIN) {
			view.pasting = 1;
			continue;
		}
		if (key == HISTORY_PASTE_END) {
			view.pasting = 0;
			dirty = 1;
			continue;
		}
		if (view.pasting && !view.searching)
			continue;
		if (view.searching) {
			if (view.pasting && (key < 32 || key > 255))
				continue;
			if (key == 27 || key == '\r' || key == '\n') view.searching = 0;
			else if (key == 0x7f || key == 8) {
				const char *end = utf8_prev_codepoint(view.query.data,
					view.query.data + view.query.len);

				(void)morph_buf_truncate(&view.query, (size_t)(end - view.query.data));
			} else if (key == 21) morph_buf_reset(&view.query);
			else if (key >= 32 && key <= 255 && view.query.len < HISTORY_QUERY_MAX - 1)
				rc = morph_buf_putc(&view.query, (char)key);
			view.focus = 0;
			if (rc == 0) rc = history_filter(&view);
			dirty = !view.pasting;
			continue;
		}
		if (key == 27 || key == 'q') {
			if (!view.detail)
				break;
			view.detail = 0;
			dirty = 1;
			continue;
		}
		if ((!view.detail && (key == '\r' || key == '\n') && history_selected(&view)) ||
		    key == 's' || (view.detail == 1 && key == 'v')) {
			if (key == 's') view.detail = 2;
			else if (key == 'v') view.stored = !view.stored;
			else { view.detail = 1; view.stored = 0; }
			view.scroll = 0;
			rc = history_document(&view);
			if (rc == 0) rc = history_wrap(&view);
			dirty = 1;
			continue;
		}
		if (!view.detail && key == '/') {
			view.searching = 1;
			dirty = 1;
			continue;
		}
		if (!view.detail && key == '\t') {
			view.filter = (view.filter + 1) % 4;
			view.focus = 0;
			rc = history_filter(&view);
			dirty = 1;
			continue;
		}
		size_t *position = view.detail ? &view.scroll : &view.focus;
		size_t count = view.detail ? view.lines.nelts : view.matches.nelts;
		size_t step = key == HISTORY_PAGE_UP || key == HISTORY_PAGE_DOWN ?
			(size_t)history_page_size(&view) : 1;
		size_t old = *position;

		if (key == HISTORY_UP || key == 'k' || key == HISTORY_PAGE_UP)
			*position = *position > step ? *position - step : 0;
		if ((key == HISTORY_DOWN || key == 'j' || key == HISTORY_PAGE_DOWN) && count)
			*position = *position + step < count ? *position + step : count - 1;
		if (key == HISTORY_HOME) *position = 0;
		if (key == HISTORY_END) *position = count ? count - 1 : 0;
		dirty = old != *position;
	}
	if (entered) {
		if (screen_active) {
			fprintf(stdout, "\033[?2004l\033[0m\033[?25h\033[?1049l");
			fflush(stdout);
		}
		if (tcsetattr(STDIN_FILENO, TCSANOW, &saved) != 0 && rc == 0)
			rc = -errno;
	}
	while (installed > 0) {
		installed--;
		(void)sigaction(signals[installed], &previous[installed], NULL);
	}
	morph_array_cleanup(&view.matches);
	morph_array_cleanup(&view.lines);
	morph_buf_cleanup(&view.document);
	morph_buf_cleanup(&view.previous);
	morph_buf_cleanup(&view.query);
	if (caught)
		raise(caught);
	MORPH_RETURN(rc);
}

void cli_history_print(const cJSON *report, int limit, int all)
{
	const cJSON *entries = cJSON_GetObjectItemCaseSensitive(report, "entries");
	int count = cJSON_GetArraySize(entries);
	int first = !all && count > limit ? count - limit : 0;
	morph_buf_t line;

	if (morph_buf_init(&line, 512) != 0)
		return;

	printf("History · %d saved entries · showing %d\n", count, count - first);
	printf("Next turn (estimated): %lld replay items · %lld stored history tokens\n",
		(long long)history_number(report, "active_items"),
		(long long)history_number(report, "history_tokens"));
	printf("History only; instructions, memory, tool definitions and new input are additional.\n"
		"Selection may change after validation or compaction before send.\n");
	for (int i = first; i < count; i++) {
		const cJSON *entry = cJSON_GetArrayItem(entries, i);
		char preview[HISTORY_PREVIEW_MAX + 1];

		history_preview(entry, preview, sizeof(preview));
		morph_buf_reset(&line);
		if (morph_buf_printf(&line, "[#%lld %s] %s %s · %lld tokens · %s",
			(long long)history_number(entry, "sequence_no"), history_string(entry, "status"),
			history_label(entry), history_string(entry, "tool_name"),
			(long long)history_number(entry, "token_count"), preview) != 0)
			break;
		(void)cli_print_untrusted_text(morph_buf_cstr(&line), UTF8_TERMINAL_TEXT_SINGLE_LINE);
		putchar('\n');
	}
	morph_buf_cleanup(&line);
	printf("IN direct · VIA SUMMARY condensed · OUT not sent · LEGACY not yet migrated\n"
		"/history --entry N [--stored] · /history --last-request · /history --messages\n");
}

int cli_history_entry_print(struct cli_context *ctx, const cJSON *report,
	int64_t sequence, int stored)
{
	cJSON *entry;
	morph_buf_t text;
	int rc = morph_buf_init(&text, BUFSIZ);

	if (rc != 0)
		return rc;
	rc = -ENOENT;
	cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(report, "entries")) {
		if (history_number(entry, "sequence_no") == sequence) {
			rc = history_entry_document(ctx, entry, stored, &text);
			break;
		}
	}
	if (rc == 0)
		(void)cli_print_untrusted_text(morph_buf_cstr(&text), UTF8_TERMINAL_TEXT_MULTILINE);
	if (rc == 0)
		putchar('\n');
	morph_buf_cleanup(&text);
	MORPH_RETURN(rc);
}

int cli_history_request_print(struct cli_context *ctx)
{
	morph_buf_t text;
	int rc = morph_buf_init(&text, BUFSIZ);

	if (rc != 0)
		return rc;
	rc = history_request_document(ctx, &text);
	if (rc == 0)
		(void)cli_print_untrusted_text(morph_buf_cstr(&text), UTF8_TERMINAL_TEXT_MULTILINE);
	if (rc == 0)
		putchar('\n');
	morph_buf_cleanup(&text);
	MORPH_RETURN(rc);
}
