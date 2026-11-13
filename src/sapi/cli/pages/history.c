#include "sapi/cli/pages/history.h"
#include "nb/err.h"
#include "runtime/runtime.h"
#include "cJSON.h"
#include "sapi/cli/presentation/output.h"
#include <morph/tui/components.h>
#include "sapi/cli/application_internal.h"
#include "sapi/cli/application.h"
#include "sapi/cli/widgets/list.h"

#define CLI_HISTORY_QUERY_MAX 256
#define CLI_HISTORY_PREVIEW_MAX 240

struct cli_history_view {
	struct cli_application *ctx;
	cJSON *report;
	nb_vec matches;
	struct morph_tui_widget_text_layout document_view;
	nb_buf document;
	struct morph_tui_frame frame;
	struct morph_tui_widget_editor_state query;
	struct morph_tui_widget_list_state selection;
	size_t scroll;
	int filter;
	int searching;
	int detail;
	int stored;
	int rows;
	int columns;
	const char *message;
};

static const char *cli_history_string(const cJSON *object, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);

	return cJSON_IsString(value) ? value->valuestring : "";
}

static int64_t cli_history_number(const cJSON *object, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);

	return cJSON_IsNumber(value) ? (int64_t)value->valuedouble : 0;
}

static const char *cli_history_label(const cJSON *entry)
{
	const char *kind = cli_history_string(entry, "kind");

	if (strcmp(kind, "user_message") == 0)
		return "You";
	if (strcmp(kind, "assistant_message") == 0)
		return "Assistant";
	if (strcmp(kind, "assistant_tool_calls") == 0)
		return "Tool call";
	if (strcmp(kind, "tool_result") == 0)
		return "Tool result";
	if (strcmp(kind, "compaction_summary") == 0)
		return "Summary";
	if (strcmp(kind, "background_receipt") == 0)
		return "Background";
	return kind;
}

static int cli_history_matches_filter(const cJSON *entry, int filter)
{
	const char *status = cli_history_string(entry, "status");

	return filter == 0 || (filter == 1 && strncmp(status, "IN", 2) == 0) ||
	       (filter == 2 && strcmp(status, "VIA SUMMARY") == 0) ||
	       (filter == 3 && strcmp(status, "OUT") == 0);
}

static int cli_history_contains(const char *text, const char *query)
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

static int cli_history_filter(struct cli_history_view *view)
{
	cJSON *entry;
	const char *query = nb_buf_cstr(&view->query.text);

	nb_vec_clear(&view->matches);
	cJSON_ArrayForEach (entry, cJSON_GetObjectItemCaseSensitive(view->report, "entries")) {
		char sequence[32];

		snprintf(sequence, sizeof(sequence), "#%lld",
			 (long long)cli_history_number(entry, "sequence_no"));
		if (!cli_history_matches_filter(entry, view->filter))
			continue;
		if (!cli_history_contains(cli_history_string(entry, "content"), query) &&
		    !cli_history_contains(cli_history_string(entry, "payload_json"), query) &&
		    !cli_history_contains(cli_history_string(entry, "tool_name"), query) &&
		    !cli_history_contains(cli_history_string(entry, "tool_call_id"), query) &&
		    !cli_history_contains(sequence, query) &&
		    !cli_history_contains(cli_history_label(entry), query))
			continue;
		cJSON **slot = nb_vec_push(&view->matches);

		if (!slot)
			return NB_ERR_NOMEM;
		*slot = entry;
	}
	morph_tui_widget_list_state_layout(&view->selection, view->matches.len,
					   view->selection.page);
	return 0;
}

static cJSON *cli_history_selected(struct cli_history_view *view)
{
	cJSON **slot = nb_vec_at(&view->matches, view->selection.index);

	return slot ? *slot : NULL;
}

static void cli_history_preview(const cJSON *entry, char *preview, size_t capacity)
{
	const char *content = cli_history_string(entry, "content");
	cJSON *payload = NULL;
	nb_buf call NB_CLEANUP(nb_buf_cleanup) = {0};
	char *limited;

	if (strcmp(cli_history_string(entry, "kind"), "assistant_tool_calls") == 0) {
		payload = cJSON_Parse(cli_history_string(entry, "payload_json"));
		cJSON *calls = cJSON_GetObjectItemCaseSensitive(payload, "calls");
		cJSON *first = cJSON_GetArrayItem(calls, 0);
		const char *arguments = cli_history_string(first, "arguments");

		if (!*arguments)
			arguments = cli_history_string(first, "input");
		char *short_arguments = nb_utf8_ellipsize_bytes(arguments, CLI_HISTORY_PREVIEW_MAX);

		if (first && short_arguments &&
		    (nb_buf_init(&call), nb_buf_reserve(&call, 128) ? 0 : NB_ERR_NOMEM) == 0) {
			(void)nb_buf_printf(&call, "%s %s", cli_history_string(first, "name"),
					    short_arguments);
			if (cJSON_GetArraySize(calls) > 1)
				(void)nb_buf_printf(&call, " (+%d calls)",
						    cJSON_GetArraySize(calls) - 1);
			content = nb_buf_cstr(&call);
		}
		free(short_arguments);
	}
	if (!*content)
		content = cli_history_string(entry, "payload_json");
	limited = nb_utf8_ellipsize_bytes(content, CLI_HISTORY_PREVIEW_MAX);
	cli_list_compact_text(preview, capacity, limited ? limited : "");
	free(limited);
	nb_buf_cleanup(&call);
	cJSON_Delete(payload);
}

static int cli_history_json_text(const cJSON *json, nb_buf *text)
{
	char *pretty = cJSON_Print(json);
	int rc;

	if (!pretty)
		return NB_ERR_NOMEM;
	rc = (nb_buf_append_cstr(text, pretty) ? 0 : NB_ERR_NOMEM);
	free(pretty);
	return rc;
}

static int cli_history_request_document(struct cli_application *ctx, nb_buf *text)
{
	cJSON *snapshot = NULL;
	int rc = runtime_session_last_request_copy(ctx->runtime, &snapshot);

	if (rc == NB_ERR_NO_DATA)
		return (nb_buf_append_cstr(
				text, "No captured request for this session.\n"
				      "Snapshots are recorded for new structured model calls.\n")
				? 0
				: NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = (nb_buf_printf(text,
			    "Last submitted model input\nModel: %s / %s\n"
			    "Turn: %s\nCaptured at: %lld (Unix time)\n\n"
			    "Captured before dispatch; provider acceptance is not implied.\n"
			    "Adapter-normalized input; credentials redacted, headers and "
			    "generation options omitted.\n\n",
			    cli_history_string(snapshot, "provider"),
			    cli_history_string(snapshot, "model"),
			    cli_history_string(snapshot, "turn_id"),
			    (long long)cli_history_number(snapshot, "created_at"))
		      ? 0
		      : NB_ERR_NOMEM);
	cJSON *input = cJSON_Parse(cli_history_string(snapshot, "input_json"));

	if (rc == 0)
		rc = input ? cli_history_json_text(input, text) : NB_ERR_PARSE;
	cJSON_Delete(input);
	cJSON_Delete(snapshot);
	return rc;
}

static int cli_history_entry_document(struct cli_application *ctx, const cJSON *entry, int stored,
				      nb_buf *text)
{
	int64_t sequence = cli_history_number(entry, "sequence_no");
	int rc = (nb_buf_printf(text,
				"#%lld  %s  %s\nNext turn: %s · %lld stored tokens\n"
				"%s\nTurn: %s\nCall ID: %s\n\n%s\n\n",
				(long long)sequence, cli_history_label(entry),
				cli_history_string(entry, "tool_name"),
				cli_history_string(entry, "status"),
				(long long)cli_history_number(entry, "token_count"),
				cli_history_string(entry, "reason"),
				cli_history_string(entry, "turn_id"),
				cli_history_string(entry, "tool_call_id"),
				stored ? "Stored view" : "Estimated request view")
			  ? 0
			  : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;
	if (stored) {
		rc = (nb_buf_append_cstr(text, cli_history_string(entry, "content"))
			      ? 0
			      : NB_ERR_NOMEM);
		const char *payload = cli_history_string(entry, "payload_json");

		if (rc == 0 && *payload) {
			cJSON *json = cJSON_Parse(payload);

			rc = (nb_buf_append_cstr(text, "\n\nStored metadata\n") ? 0 : NB_ERR_NOMEM);
			if (rc == 0)
				rc = json ? cli_history_json_text(json, text)
					  : (nb_buf_append_cstr(text, payload) ? 0 : NB_ERR_NOMEM);
			cJSON_Delete(json);
		}
		return rc;
	}
	cJSON *input = NULL;

	rc = runtime_session_history_entry_input_copy(ctx->runtime, sequence, &input);
	if (rc == NB_ERR_NO_DATA || rc == NB_ERR_NOT_FOUND)
		return (nb_buf_append_cstr(
				text,
				"Original entry is not directly sent in the current window.\n"
				"Use Stored view for retained content. VIA SUMMARY entries are "
				"represented by summaries.\n"
				"LEGACY entries require migration and validation before replay.\n")
				? 0
				: NB_ERR_NOMEM);
	if (rc == 0)
		rc = cli_history_json_text(input, text);
	cJSON_Delete(input);
	return rc;
}

static int cli_history_document(struct cli_history_view *view)
{
	struct cli_application *ctx = view->ctx;
	(void)ctx;
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&text), nb_buf_reserve(&text, BUFSIZ) ? 0 : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;
	if (view->detail == 2)
		rc = cli_history_request_document(view->ctx, &text);
	else if (cli_history_selected(view))
		rc = cli_history_entry_document(view->ctx, cli_history_selected(view), view->stored,
						&text);
	char *safe = rc == 0 ? morph_terminal_sanitize_dup(nb_buf_cstr(&text), text.len,
							   MORPH_TERMINAL_TEXT_MULTILINE, NULL)
			     : NULL;

	if (rc == 0 && !safe)
		rc = NB_ERR_NOMEM;
	nb_buf_clear(&view->document);
	if (rc == 0) {
		for (char *p = safe; *p; p++) {
			if (*p == '\t')
				*p = ' ';
		}
		rc = (nb_buf_append_cstr(&view->document, safe) ? 0 : NB_ERR_NOMEM);
	}
	free(safe);
	nb_buf_cleanup(&text);
	return rc;
}

static int cli_history_wrap(struct cli_history_view *view)
{
	size_t width = view->columns > 4 ? (size_t)view->columns - 4 : 1;

	return morph_tui_widget_text_layout_wrap_borrow(&view->document_view,
							nb_buf_cstr(&view->document), width);
}

static int cli_history_frame_line(struct cli_application *ctx, nb_buf *frame, int columns,
				  const char *text, const char *style)
{
	char clipped[BUFSIZ];
	char *safe = morph_terminal_sanitize_dup(text, strlen(text),
						 MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
	int rc;

	if (!safe)
		return NB_ERR_NOMEM;
	morph_terminal_copy_ellipsized_display_width(clipped, sizeof(clipped), safe,
						     (size_t)(columns > 1 ? columns - 1 : 0), 0);
	free(safe);
	rc = (nb_buf_printf(frame, "%s%s%s\n", cli_output_color_enabled(ctx) ? style : "", clipped,
			    cli_output_color_enabled(ctx) ? ANSI_RESET : "")
		      ? 0
		      : NB_ERR_NOMEM);
	return rc;
}

static int cli_history_page_size(const struct cli_history_view *view)
{
	struct morph_tui_terminal_rect area = {1, 1, view->columns, view->rows};

	(void)morph_tui_terminal_rect_take_top(&area, view->detail ? 2 : 8);
	struct morph_tui_terminal_rect body =
		morph_tui_terminal_rect_take_top(&area, area.height - 3);

	return body.height > 0 ? body.height : 1;
}

static int cli_history_render(struct cli_history_view *view)
{
	nb_buf frame NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&frame), nb_buf_reserve(&frame, BUFSIZ) ? 0 : NB_ERR_NOMEM);

	if (rc == 0)
		rc = (nb_buf_init(&text), nb_buf_reserve(&text, BUFSIZ) ? 0 : NB_ERR_NOMEM);
	if (rc != 0) {
		nb_buf_cleanup(&frame);
		return rc;
	}
	int page = cli_history_page_size(view);
	morph_tui_widget_list_state_layout(&view->selection, view->matches.len, (size_t)page);
	size_t first = view->selection.first;
	view->scroll = morph_tui_widget_scroll_update(view->scroll, view->document_view.lines.len,
						      (size_t)page, 0);
	for (int row = 1; row <= view->rows && rc == 0; row++) {
		const char *style = ANSI_DIM;

		nb_buf_clear(&text);
		if (view->columns < 32 || view->rows < 12) {
			if (row == 1)
				(void)nb_buf_append_cstr(&text, "Widen terminal · Esc back");
		} else if (row == 1) {
			(void)nb_buf_append_cstr(
				&text, view->detail == 2
					       ? "  morph / history / last submitted input"
				       : view->detail ? "  morph / history / entry details"
						      : "  morph / history");
			style = ANSI_BOLD;
		} else if (view->detail && row >= 3 && row < 3 + page) {
			struct morph_tui_widget_text_line *line = nb_vec_at(
				&view->document_view.lines, view->scroll + (size_t)(row - 3));

			if (line) {
				(void)nb_buf_append_cstr(&text, "  ");
				(void)nb_buf_append(&text, view->document.ptr + line->offset,
						    line->length);
				style = "";
			}
		} else if (view->detail && row == view->rows - 1) {
			(void)nb_buf_printf(&text, "  Lines %zu–%zu of %zu", view->scroll + 1,
					    view->scroll + (size_t)page <
							    view->document_view.lines.len
						    ? view->scroll + (size_t)page
						    : view->document_view.lines.len,
					    view->document_view.lines.len);
		} else if (view->detail && row == view->rows) {
			(void)nb_buf_append_cstr(
				&text,
				view->columns < 76
					? (view->detail == 2 ? "  Up/Down PgUp/PgDn · Esc back"
							     : "  v view · PgUp/PgDn · Esc back")
				: view->detail == 2 ? "  Up/Down PgUp/PgDn scroll · Esc back"
						    : "  v request/stored · s last request · "
						      "PgUp/PgDn scroll · Esc back");
		} else if (!view->detail && row == 2) {
			(void)nb_buf_printf(
				&text, "  %d saved entries · %zu matching",
				cJSON_GetArraySize(cJSON_GetObjectItem(view->report, "entries")),
				view->matches.len);
			if (view->columns >= 76)
				(void)nb_buf_printf(&text, " · %s",
						    cli_history_string(view->report, "model"));
		} else if (!view->detail && row == 4) {
			if (view->columns < 76)
				(void)nb_buf_printf(
					&text, "  Next (est): %lld items · %lld tok",
					(long long)cli_history_number(view->report, "active_items"),
					(long long)cli_history_number(view->report,
								      "history_tokens"));
			else
				(void)nb_buf_printf(
					&text,
					"  Next turn · estimated history: %lld tokens / %lld limit",
					(long long)cli_history_number(view->report,
								      "history_tokens"),
					(long long)cli_history_number(view->report,
								      "context_limit"));
		} else if (!view->detail && row == 5) {
			(void)nb_buf_append_cstr(
				&text,
				cJSON_IsTrue(cJSON_GetObjectItem(view->report, "needs_repair"))
					? "  Replay needs repair before send; current membership "
					  "may change."
				: view->columns < 76 ? "  History only; prompt/tools are extra."
						     : "  History only; instructions, tools and "
						       "new input are additional.");
		} else if (!view->detail && row == 6) {
			const char *filters[] = {"All", "Included", "Summarized", "Excluded"};

			if (view->columns < 50)
				(void)nb_buf_printf(&text, "  Filter: %s (Tab)",
						    filters[view->filter]);
			else {
				for (int i = 0; i < 4; i++)
					(void)nb_buf_printf(
						&text, "  %s%s%s", i == view->filter ? "[" : "",
						filters[i], i == view->filter ? "]" : "");
			}
		} else if (!view->detail && row == 8) {
			(void)nb_buf_printf(&text, "  ENTRY / ACTIVITY%*sNEXT TURN",
					    view->columns - 37, "");
		} else if (!view->detail && row >= 9 && row < 9 + page) {
			size_t index = first + (size_t)(row - 9);
			cJSON **slot = nb_vec_at(&view->matches, index);

			if (slot) {
				char preview[CLI_HISTORY_PREVIEW_MAX + 1];
				char clipped[BUFSIZ];
				int left = view->columns - 20;
				cJSON *entry = *slot;

				cli_history_preview(entry, preview, sizeof(preview));
				(void)nb_buf_printf(
					&text, "%s #%lld %s %s %s",
					index == view->selection.index ? ">" : " ",
					(long long)cli_history_number(entry, "sequence_no"),
					cli_history_label(entry),
					cli_history_string(entry, "tool_name"), preview);
				char *safe = morph_terminal_sanitize_dup(
					nb_buf_cstr(&text), text.len,
					MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);

				morph_terminal_copy_ellipsized_display_width(
					clipped, sizeof(clipped), safe ? safe : "", (size_t)left,
					0);
				free(safe);
				nb_buf_clear(&text);
				(void)nb_buf_printf(&text, "%s%*s%s", clipped,
						    left - (int)nb_utf8_width_cstr(clipped) + 1, "",
						    cli_history_string(entry, "status"));
				const char *status = cli_history_string(entry, "status");

				style = index == view->selection.index ? ANSI_CYAN ANSI_BOLD
					: strcmp(status, "OUT") == 0   ? ANSI_DIM
					: strcmp(status, "IN") == 0    ? ""
								       : ANSI_YELLOW;
			} else if (!view->matches.len && row == 9) {
				(void)nb_buf_append_cstr(&text, "  No matching history entries.");
			}
		} else if (!view->detail && row == view->rows - 2) {
			(void)nb_buf_append_cstr(
				&text, "  IN direct · VIA SUMMARY condensed · OUT not sent");
		} else if (!view->detail && row == view->rows - 1) {
			(void)nb_buf_printf(&text, "  %s%s%s", view->searching ? "Search: " : "/ ",
					    nb_buf_cstr(&view->query.text),
					    view->message ? view->message : "");
		} else if (!view->detail && row == view->rows) {
			(void)nb_buf_append_cstr(
				&text, view->columns < 70 ? "  Up/Down Enter / Tab s Esc back"
							  : "  Up/Down · Enter details · / search "
							    "· Tab filter · s request · Esc back");
		}
		if (nb_buf_status(&text))
			rc = NB_ERR_NOMEM;
		else
			rc = cli_history_frame_line(view->ctx, &frame, view->columns,
						    nb_buf_cstr(&text), style);
	}
	if (rc == 0)
		rc = morph_tui_frame_present(&view->frame, stdout, view->columns, view->rows,
					     nb_buf_cstr(&frame));
	nb_buf_cleanup(&text);
	nb_buf_cleanup(&frame);
	return rc;
}

static int cli_history_draw(void *data, const struct morph_tui_screen *screen)
{
	struct cli_history_view *view = data;
	int resized = view->rows != screen->area.height || view->columns != screen->area.width;

	view->rows = screen->area.height;
	view->columns = screen->area.width;
	if (resized && view->detail) {
		int rc = cli_history_wrap(view);

		if (rc != 0)
			return rc;
	}
	return cli_history_render(view);
}

static int cli_history_update(void *data, const struct morph_tui_event *event)
{
	struct cli_history_view *view = data;

	if (event->type != MORPH_TUI_EVENT_KEY)
		return MORPH_TUI_SCREEN_CONTINUE;
	int key = event->key;

	if (key == MORPH_TUI_KEY_PASTE_BEGIN || key == MORPH_TUI_KEY_PASTE_END) {
		(void)morph_tui_widget_editor_state_update(&view->query, key);
		return key == MORPH_TUI_KEY_PASTE_END ? MORPH_TUI_SCREEN_REDRAW
						      : MORPH_TUI_SCREEN_CONTINUE;
	}
	if (event->pasting && !view->searching)
		return MORPH_TUI_SCREEN_CONTINUE;
	if (!event->pasting && (key == 3 || key == 4))
		return MORPH_TUI_SCREEN_CLOSE;
	if (view->searching) {
		int result = morph_tui_widget_editor_state_update(&view->query, key);

		if (result < 0)
			return result;
		if (result == MORPH_TUI_WIDGET_SUBMIT || result == MORPH_TUI_WIDGET_BACK)
			view->searching = 0;
		view->selection.index = 0;
		int rc = cli_history_filter(view);

		if (rc != 0)
			return rc;
		return view->query.pasting ? MORPH_TUI_SCREEN_CONTINUE : MORPH_TUI_SCREEN_REDRAW;
	}
	if (key == 27 || key == 'q') {
		if (!view->detail)
			return MORPH_TUI_SCREEN_CLOSE;
		view->detail = 0;
		return MORPH_TUI_SCREEN_REDRAW;
	}
	if ((!view->detail && (key == '\r' || key == '\n') && cli_history_selected(view)) ||
	    key == 's' || (view->detail == 1 && key == 'v')) {
		if (key == 's')
			view->detail = 2;
		else if (key == 'v')
			view->stored = !view->stored;
		else {
			view->detail = 1;
			view->stored = 0;
		}
		view->scroll = 0;
		int rc = cli_history_document(view);

		if (rc == 0)
			rc = cli_history_wrap(view);
		return rc != 0 ? rc : MORPH_TUI_SCREEN_REDRAW;
	}
	if (!view->detail && key == '/') {
		view->searching = 1;
		return MORPH_TUI_SCREEN_REDRAW;
	}
	if (!view->detail && key == '\t') {
		view->filter = (view->filter + 1) % 4;
		view->selection.index = 0;
		int rc = cli_history_filter(view);

		return rc != 0 ? rc : MORPH_TUI_SCREEN_REDRAW;
	}
	if (view->detail) {
		size_t old = view->scroll;

		view->scroll =
			morph_tui_widget_scroll_update(view->scroll, view->document_view.lines.len,
						       (size_t)cli_history_page_size(view), key);
		return old != view->scroll ? MORPH_TUI_SCREEN_REDRAW : MORPH_TUI_SCREEN_CONTINUE;
	}
	return morph_tui_widget_list_state_update(&view->selection, key)
		       ? MORPH_TUI_SCREEN_REDRAW
		       : MORPH_TUI_SCREEN_CONTINUE;
}

static void cli_history_close(void *data, int result)
{
	struct cli_history_view *view = data;

	(void)result;
	nb_vec_cleanup(&view->matches);
	morph_tui_widget_text_layout_cleanup(&view->document_view);
	nb_buf_cleanup(&view->document);
	morph_tui_frame_cleanup(&view->frame);
	morph_tui_widget_editor_state_cleanup(&view->query);
	cJSON_Delete(view->report);
	free(view);
}

int cli_history_view(struct cli_application *ctx, cJSON *report)
{
	struct cli_history_view *view = calloc(1, sizeof(*view));
	struct morph_tui_screen screen = {0};
	cJSON *entry;
	const struct morph_tui_screen_ops ops = {.update = cli_history_update,
						 .draw = cli_history_draw};
	if (!view)
		return NB_ERR_NOMEM;
	*view = (struct cli_history_view){.ctx = ctx, .rows = 24, .columns = 80};
	view->report = cJSON_Duplicate(report, 1);
	int rc = view->report ? (nb_vec_init(&view->matches, sizeof(entry)),
				 nb_vec_reserve(&view->matches, 32) ? 0 : NB_ERR_NOMEM)
			      : NB_ERR_NOMEM;

	view->selection.flags = MORPH_TUI_WIDGET_LIST_VIM;
	if (rc == 0)
		rc = (nb_buf_init(&view->document),
		      nb_buf_reserve(&view->document, BUFSIZ) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = morph_tui_widget_editor_state_init(&view->query, "", CLI_HISTORY_QUERY_MAX, 0);
	if (rc == 0)
		rc = cli_history_filter(view);
	if (rc == 0 && view->matches.len)
		view->selection.index = view->matches.len - 1;
	if (!rc && cli_application_tui_borrow(ctx)) {
		rc = cli_application_present_screen(ctx, &ops, view, cli_history_close);
		if (!rc)
			return 0;
	}
	if (rc) {
		cli_history_close(view, rc);
		return rc;
	}
	nb_atomic_int_store(&ctx->interrupted, 0);
	screen.interrupted = &ctx->interrupted;
	if (rc == 0)
		rc = morph_tui_screen_open(&screen, stdin, stdout,
					   MORPH_TUI_TERMINAL_SESSION_ALTERNATE |
						   MORPH_TUI_TERMINAL_SESSION_PASTE);
	if (rc == 0)
		rc = morph_tui_screen_run(&screen, &ops, view);
	int caught = nb_atomic_int_exchange(&ctx->interrupted, 0);
	int leave_rc = morph_tui_screen_close(&screen);

	if (caught && rc == NB_ERR_INTERRUPTED)
		rc = 0;
	if (rc == 0)
		rc = leave_rc;
	cli_history_close(view, rc);
	return rc;
}

void cli_history_print(struct cli_application *ctx, const cJSON *report, int limit, int all)
{
	const cJSON *entries = cJSON_GetObjectItemCaseSensitive(report, "entries");
	int count = cJSON_GetArraySize(entries);
	int first = !all && count > limit ? count - limit : 0;
	nb_buf line NB_CLEANUP(nb_buf_cleanup) = {0};

	if ((nb_buf_init(&line), nb_buf_reserve(&line, 512) ? 0 : NB_ERR_NOMEM) != 0)
		return;

	cli_output_printf(ctx, "History · %d saved entries · showing %d\n", count, count - first);
	cli_output_printf(ctx, "Next turn (estimated): %lld replay items · %lld stored history tokens\n",
	       (long long)cli_history_number(report, "active_items"),
	       (long long)cli_history_number(report, "history_tokens"));
	cli_output_printf(ctx, "History only; instructions, memory, tool definitions and new input are "
		    "additional.\n"
		    "Selection may change after validation or compaction before send.\n");
	for (int i = first; i < count; i++) {
		const cJSON *entry = cJSON_GetArrayItem(entries, i);
		char preview[CLI_HISTORY_PREVIEW_MAX + 1];

		cli_history_preview(entry, preview, sizeof(preview));
		nb_buf_clear(&line);
		if ((nb_buf_printf(&line, "[#%lld %s] %s %s · %lld tokens · %s",
				   (long long)cli_history_number(entry, "sequence_no"),
				   cli_history_string(entry, "status"), cli_history_label(entry),
				   cli_history_string(entry, "tool_name"),
				   (long long)cli_history_number(entry, "token_count"), preview)
			     ? 0
			     : NB_ERR_NOMEM) != 0)
			break;
		(void)cli_output_print_untrusted(ctx, nb_buf_cstr(&line),
						 MORPH_TERMINAL_TEXT_SINGLE_LINE);
		cli_output_putchar(ctx, '\n');
	}
	nb_buf_cleanup(&line);
	cli_output_printf(ctx,
	       "IN direct · VIA SUMMARY condensed · OUT not sent · LEGACY not yet migrated\n"
	       "/history --entry N [--stored] · /history --last-request · /history --messages\n");
}

int cli_history_entry_print(struct cli_application *ctx, const cJSON *report, int64_t sequence,
			    int stored)
{
	cJSON *entry;
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&text), nb_buf_reserve(&text, BUFSIZ) ? 0 : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;
	rc = NB_ERR_NOT_FOUND;
	cJSON_ArrayForEach (entry, cJSON_GetObjectItemCaseSensitive(report, "entries")) {
		if (cli_history_number(entry, "sequence_no") == sequence) {
			rc = cli_history_entry_document(ctx, entry, stored, &text);
			break;
		}
	}
	if (rc == 0)
		(void)cli_output_print_untrusted(ctx, nb_buf_cstr(&text),
						 MORPH_TERMINAL_TEXT_MULTILINE);
	if (rc == 0)
		cli_output_putchar(ctx, '\n');
	nb_buf_cleanup(&text);
	return rc;
}

int cli_history_request_print(struct cli_application *ctx)
{
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&text), nb_buf_reserve(&text, BUFSIZ) ? 0 : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;
	rc = cli_history_request_document(ctx, &text);
	if (rc == 0)
		(void)cli_output_print_untrusted(ctx, nb_buf_cstr(&text),
						 MORPH_TERMINAL_TEXT_MULTILINE);
	if (rc == 0)
		cli_output_putchar(ctx, '\n');
	nb_buf_cleanup(&text);
	return rc;
}
