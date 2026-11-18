#include "sapi/cli/page/history.h"
#include <nb/err.h>
#include "runtime/runtime.h"
#include "cJSON.h"
#include "sapi/cli/presentation/output.h"
#include <nothing/tui/controls/list_view.h>
#include <nothing/tui/controls/text_field.h>
#include <nothing/tui/terminal/text.h>
#include <nothing/tui/views/stack_view.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/document_view.h>
#include <nothing/tui/view_controller.h>
#include "sapi/cli/application_internal.h"
#include "sapi/cli/application.h"
#include "sapi/cli/component/list.h"

#define CLI_HISTORY_QUERY_MAX 256
#define CLI_HISTORY_PREVIEW_MAX 240

struct cli_history_view {
	struct cli_application *ctx;
	struct nt_view_controller *controller;
	struct nt_view *root, *overview, *reader, *narrow, *empty, *title, *count, *budget, *scope,
		*filters, *heading, *list, *query, *help, *document_view, *range, *detail_help;
	cJSON *report;
	nb_vec matches;
	nb_buf document;
	int columns, rows;
	int filter, searching, detail, stored;
	char row[BUFSIZ];
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
	const char *query = nt_text_field_text(view->query);

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
	return 0;
}

static cJSON *cli_history_selected(struct cli_history_view *view)
{
	cJSON **slot = nb_vec_at(&view->matches, nt_list_view_selected(view->list));

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

static const char *cli_history_row(void *data, size_t index)
{
	struct cli_history_view *page = data;
	cJSON **item = nb_vec_at(&page->matches, index);
	if (!item)
		return "";
	char preview[CLI_HISTORY_PREVIEW_MAX + 1];
	char title[BUFSIZ], clipped[BUFSIZ];
	cli_history_preview(*item, preview, sizeof(preview));
	snprintf(title, sizeof(title), "%c #%lld %s %s %s",
		 index == nt_list_view_selected(page->list) ? '>' : ' ',
		 (long long)cli_history_number(*item, "sequence_no"), cli_history_label(*item),
		 cli_history_string(*item, "tool_name"), preview);
	int left = page->columns > 20 ? page->columns - 20 : 1;
	nt_terminal_copy_ellipsized_display_width(clipped, sizeof(clipped), title, (size_t)left, 0);
	int gap = left - (int)nb_utf8_width_cstr(clipped) + 1;
	snprintf(page->row, sizeof(page->row), "%s%*s%s", clipped, gap, "",
		 cli_history_string(*item, "status"));
	return page->row;
}

static void cli_history_row_style(void *data, size_t index, int selected,
				  struct nt_text_style *style)
{
	struct cli_history_view *page = data;
	*style = (struct nt_text_style){0};
	if (!cli_output_color_enabled(page->ctx))
		return;
	cJSON **item = nb_vec_at(&page->matches, index);
	const char *status = item ? cli_history_string(*item, "status") : "";
	if (selected) {
		style->attributes = NT_STYLE_BOLD;
		style->foreground = (struct nt_color){NT_COLOR_INDEXED, 6};
	} else if (!strcmp(status, "OUT"))
		style->attributes = NT_STYLE_DIM;
	else if (strcmp(status, "IN"))
		style->foreground = (struct nt_color){NT_COLOR_INDEXED, 3};
}

static int cli_history_summary(struct cli_history_view *page)
{
	char text[BUFSIZ];
	int count = cJSON_GetArraySize(cJSON_GetObjectItem(page->report, "entries"));
	snprintf(text, sizeof(text), "  %d saved entries · %zu matching%s%s", count,
		 page->matches.len, page->columns >= 76 ? " · " : "",
		 page->columns >= 76 ? cli_history_string(page->report, "model") : "");
	int rc = nt_label_set_text(page->count, text);
	const char *filters[] = {"All", "Included", "Summarized", "Excluded"};
	nb_buf names NB_CLEANUP(nb_buf_cleanup) = {0};
	if (page->columns < 50)
		nb_buf_printf(&names, "  Filter: %s (Tab)", filters[page->filter]);
	else
		for (int i = 0; i < 4; i++)
			nb_buf_printf(&names, "  %s%s%s", i == page->filter ? "[" : "", filters[i],
				      i == page->filter ? "]" : "");
	if (!rc)
		rc = nb_buf_status(&names);
	if (!rc)
		rc = nt_label_set_text(page->filters, nb_buf_cstr(&names));
	if (page->columns < 76)
		snprintf(text, sizeof(text), "  Next (est): %lld items · %lld tok",
			 (long long)cli_history_number(page->report, "active_items"),
			 (long long)cli_history_number(page->report, "history_tokens"));
	else
		snprintf(text, sizeof(text),
			 "  Next turn · estimated history: %lld tokens / %lld limit",
			 (long long)cli_history_number(page->report, "history_tokens"),
			 (long long)cli_history_number(page->report, "context_limit"));
	if (!rc)
		rc = nt_label_set_text(page->budget, text);
	if (!rc)
		rc = nt_label_set_text(
			page->scope, cJSON_IsTrue(cJSON_GetObjectItem(page->report, "needs_repair"))
					     ? "  Replay needs repair before send; current "
					       "membership may change."
				     : page->columns < 76
					     ? "  History only; prompt/tools are extra."
					     : "  History only; instructions, tools and new input "
					       "are additional.");
	snprintf(text, sizeof(text), "  ENTRY / ACTIVITY%*sNEXT TURN",
		 page->columns > 37 ? page->columns - 37 : 0, "");
	if (!rc)
		rc = nt_label_set_text(page->heading, text);
	return rc;
}

static int cli_history_range(struct cli_history_view *page)
{
	size_t first, count, total;
	int rc = nt_document_view_visible_lines(page->document_view, &first, &count, &total);
	if (rc)
		return rc;
	char range[128];
	snprintf(range, sizeof(range), "  Lines %zu–%zu of %zu", first + 1, first + count, total);
	return nt_label_set_text(page->range, range);
}

static int cli_history_sync(struct cli_history_view *page)
{
	int small = page->columns < 32 || page->rows < 12;
	int rc = nt_view_set_hidden(page->overview, small || page->detail);
	if (!rc)
		rc = nt_view_set_hidden(page->reader, small || !page->detail);
	if (!rc)
		rc = nt_view_set_hidden(page->narrow, !small);
	if (!rc)
		rc = nt_view_set_hidden(page->empty, page->matches.len != 0);
	if (!rc)
		rc = nt_view_set_hidden(page->list, page->matches.len == 0);
	if (!rc)
		rc = nt_text_field_set_prompt(page->query, page->searching ? "  Search: " : "  / ");
	if (!rc)
		rc = cli_history_summary(page);
	if (!rc)
		rc = nt_label_set_text(page->help, page->columns < 70
							   ? "  Up/Down Enter / Tab s Esc back"
							   : "  Up/Down · Enter details · / search "
							     "· Tab filter · s request · Esc back");
	if (!rc)
		rc = nt_label_set_text(
			page->title, page->detail == 2 ? "  morph / history / last submitted input"
						       : "  morph / history / entry details");
	if (!rc)
		rc = nt_label_set_text(
			page->detail_help,
			page->columns < 76
				? (page->detail == 2 ? "  Up/Down PgUp/PgDn · Esc back"
						     : "  v view · PgUp/PgDn · Esc back")
				: (page->detail == 2 ? "  Up/Down PgUp/PgDn scroll · Esc back"
						     : "  v request/stored · s last request · "
						       "PgUp/PgDn scroll · Esc back"));
	if (!rc && page->detail)
		rc = cli_history_range(page);
	if (!rc && !small)
		rc = nt_view_focus(page->searching     ? page->query
				   : page->detail      ? page->document_view
				   : page->matches.len ? page->list
						       : page->root);
	return rc;
}

static int cli_history_reload(struct cli_history_view *page, size_t selected)
{
	int rc = cli_history_filter(page);
	if (!rc)
		rc = nt_list_view_set_source(page->list, page->matches.len, cli_history_row, page);
	if (!rc && page->matches.len)
		rc = nt_list_view_select(page->list, selected < page->matches.len
							     ? selected
							     : page->matches.len - 1);
	return rc ? rc : cli_history_sync(page);
}

static int cli_history_open_document(struct cli_history_view *page, int which)
{
	page->detail = which;
	int rc = cli_history_document(page);
	if (!rc)
		rc = nt_document_view_set_text(page->document_view, nb_buf_cstr(&page->document));
	return rc ? rc : cli_history_sync(page);
}

static int cli_history_action(void *data, const struct nt_view_event *event)
{
	struct cli_history_view *page = data;
	if (event->type == NT_VIEW_EVENT_ACTIVATE)
		return cli_history_open_document(page, 1);
	if (event->type == NT_VIEW_EVENT_CHANGED)
		return cli_history_reload(page, 0);
	if (event->type == NT_VIEW_EVENT_SUBMIT) {
		page->searching = 0;
		return cli_history_sync(page);
	}
	if (event->type == NT_VIEW_EVENT_SCROLLED || event->type == NT_VIEW_EVENT_SIZE_CHANGED)
		return cli_history_range(page);
	return 0;
}

static int cli_history_key(void *data, const struct nt_view_event *key)
{
	struct cli_history_view *page = data;
	const struct nt_event *event = key->input;
	int value = event->key;
	if (event->pasting || event->action == NT_KEY_ACTION_RELEASE)
		return 0;
	int rc = 0;
	if (value == 3 || value == 4) {
		rc = nt_view_controller_dismiss(page->controller, 0);
	} else if (page->searching) {
		if (value != 27 && value != '\r' && value != '\n')
			return 0;
		page->searching = 0;
		rc = cli_history_sync(page);
	} else if (value == 27 || value == 'q') {
		if (!page->detail)
			rc = nt_view_controller_dismiss(page->controller, 0);
		else {
			page->detail = 0;
			rc = cli_history_sync(page);
		}
	} else if (value == 's')
		rc = cli_history_open_document(page, 2);
	else if (value == 'v' && page->detail == 1) {
		page->stored = !page->stored;
		rc = cli_history_open_document(page, 1);
	} else if (value == '/' && !page->detail) {
		page->searching = 1;
		rc = cli_history_sync(page);
	} else if (value == '\t' && !page->detail) {
		page->filter = (page->filter + 1) % 4;
		rc = cli_history_reload(page, 0);
	} else
		return 0;
	return rc ? rc : NT_VIEW_EVENT_STOP;
}

static int cli_history_size(void *data, const struct nt_view_event *event)
{
	struct cli_history_view *page = data;
	if (event->target != page->root)
		return 0;
	page->columns = event->rect.width;
	page->rows = event->rect.height;
	return cli_history_sync(page);
}

static void cli_history_build(struct nt_view_controller *controller, struct nt_builder *builder)
{
	struct cli_history_view *page = controller->data;
	page->controller = controller;
	struct nt_view *root = nt_column_add(builder, NULL, 0, NULL);
	page->root = root;
	nt_builder_fail(builder, nt_view_set_focusable(root, 1));
	const struct nt_view_options fill = {.grow = 1};
	const struct nt_view_options dim = {.height = 1, .style = {.attributes = NT_STYLE_DIM}};
	const struct nt_view_options bold = {.height = 1, .style = {.attributes = NT_STYLE_BOLD}};
	page->narrow = nt_label_add(builder, root, "Widen terminal · Esc back", &dim);
	page->overview = nt_column_add(builder, root, 0, &fill);
	page->reader = nt_column_add(builder, root, 0, &fill);
	nt_label_add(builder, page->overview, "  morph / history", &bold);
	page->count = nt_label_add(builder, page->overview, "", &dim);
	nt_label_add(builder, page->overview, "", &dim);
	page->budget = nt_label_add(builder, page->overview, "", &dim);
	page->scope = nt_label_add(builder, page->overview, "", &dim);
	page->filters = nt_label_add(builder, page->overview, "", &dim);
	nt_label_add(builder, page->overview, "", &dim);
	page->heading = nt_label_add(builder, page->overview, "", &dim);
	page->list = nt_list_view_add(builder, page->overview, NULL, 0, 0, &fill);
	page->empty =
		nt_label_add(builder, page->overview, "  No matching history entries.", &fill);
	nt_label_add(builder, page->overview, "  IN direct · VIA SUMMARY condensed · OUT not sent",
		     &dim);
	page->query = nt_text_field_add(
		builder, page->overview,
		&(struct nt_text_field_options){.limit = CLI_HISTORY_QUERY_MAX}, &dim);
	page->help = nt_label_add(builder, page->overview, "", &dim);
	page->title = nt_label_add(builder, page->reader, "", &bold);
	nt_label_add(builder, page->reader, "", &dim);
	page->document_view = nt_document_view_add(
		builder, page->reader, "", NULL,
		&(struct nt_view_options){.grow = 1, .padding = {.left = 2, .right = 2}});
	nt_label_add(builder, page->reader, "", &dim);
	page->range = nt_label_add(builder, page->reader, "", &dim);
	page->detail_help = nt_label_add(builder, page->reader, "", &dim);
	if (nt_builder_error(builder))
		return;
	nt_builder_fail(builder, nt_list_view_set_navigation(page->list, NT_LIST_VIEW_VIM));
	nt_builder_fail(builder,
			nt_list_view_set_row_style(page->list, cli_history_row_style, page));
	if (!nt_view_on_full(
		    root, NT_VIEW_EVENT_KEY,
		    &(struct nt_view_listener_options){.flags = NT_VIEW_LISTEN_CAPTURE,
						       NT_VIEW_LISTENER(cli_history_key, page)}) ||
	    !nt_view_on(root, NT_VIEW_EVENT_SIZE_CHANGED, cli_history_size, page) ||
	    !nt_view_on(page->list, NT_VIEW_EVENT_ACTIVATE, cli_history_action, page) ||
	    !nt_view_on(page->query, NT_VIEW_EVENT_CHANGED, cli_history_action, page) ||
	    !nt_view_on(page->query, NT_VIEW_EVENT_SUBMIT, cli_history_action, page) ||
	    !nt_view_on(page->document_view, NT_VIEW_EVENT_SIZE_CHANGED, cli_history_action,
			page) ||
	    !nt_view_on(page->document_view, NT_VIEW_EVENT_SCROLLED, cli_history_action, page)) {
		nt_builder_fail(builder, NB_ERR_NOMEM);
		return;
	}
	nt_builder_fail(builder, cli_history_reload(page, SIZE_MAX));
}

static void cli_history_close(void *data, int result)
{
	struct cli_history_view *page = data;
	(void)result;
	nb_vec_cleanup(&page->matches);
	nb_buf_cleanup(&page->document);
	cJSON_Delete(page->report);
	free(page);
}

static void cli_history_destroy(struct nt_view_controller *controller)
{
	struct cli_history_view *page = controller->data;
	if (page)
		cli_application_panel_complete(page->ctx, page, cli_history_close,
					       nt_view_controller_result(controller));
}

int cli_history_view(struct cli_application *ctx, cJSON *report)
{
	static const struct nt_view_controller_class type = {
		.superclass = &nt_view_controller_base_class,
		.instance_size = sizeof(struct nt_view_controller),
		.load_view = cli_history_build,
		.deinit = cli_history_destroy};
	struct cli_history_view *page = calloc(1, sizeof(*page));
	if (!page)
		return NB_ERR_NOMEM;
	page->ctx = ctx;
	page->report = cJSON_Duplicate(report, 1);
	nb_vec_init(&page->matches, sizeof(report));
	nt_terminal_dimensions(STDOUT_FILENO, &page->columns, &page->rows);
	int accepted = 0;
	int rc = page->report ? cli_application_show_page(ctx, &type, page, 0, &accepted)
			      : NB_ERR_NOMEM;
	if (!accepted)
		cli_history_close(page, rc);
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
	cli_output_printf(ctx,
			  "Next turn (estimated): %lld replay items · %lld stored history tokens\n",
			  (long long)cli_history_number(report, "active_items"),
			  (long long)cli_history_number(report, "history_tokens"));
	cli_output_printf(ctx,
			  "History only; instructions, memory, tool definitions and new input are "
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
	cli_output_printf(
		ctx,
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
