#include "sapi/cli/pages/memory.h"
#include "sapi/cli/presentation/panel_style.h"

#include "sapi/cli/application.h"
#include "sapi/cli/components/list.h"
#include "sapi/cli/components/visibility.h"
#include "runtime/services.h"
#include <nb/buf.h>
#include <nb/cleanup.h>
#include "errors.h"
#include "cJSON.h"
#include <nothing/tui/view_controller.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/controls/list_view.h>
#include <nothing/tui/views/stack_view.h>
#include <nothing/tui/controls/text_field.h>
#include <nothing/tui/views/text_view.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct cli_memory_page {
	struct nt_view_controller base;
	struct cli_application *ctx;
	struct nt_view *list;
	struct nt_view *document;
	struct nt_view *search;
	cJSON *report;
	struct nt_component_state *query, *detail, *searching;
	char row[BUFSIZ];
	int presented;
};

static const char *cli_memory_string(const cJSON *object, const char *key)
{
	const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));

	return value ? value : "";
}

static const char *cli_memory_section_name(int section)
{
	static const char *const names[] = {
		"Summary",   "Your preferences", "Saved facts",	   "Reusable tips",
		"Past work", "Not applied",	 "Memory updates", "Memory preview",
	};

	return names[section];
}

static const char *cli_memory_section_description(int section)
{
	static const char *const descriptions[] = {
		"Memory status, counts and settings.",
		"How you want morph to respond.",
		"Facts remembered about you and this project.",
		"Methods and mistakes learned from past work.",
		"Earlier tasks, decisions and results.",
		"Inferred settings that are not in effect.",
		"Progress and errors while saving memories.",
		"Memory prepared for the most recent reply.",
	};

	return descriptions[section];
}

static int cli_memory_record_visible(const cJSON *entry, int section)
{
	const char *kind = cli_memory_string(entry, "kind");

	return (section == 2 && (!strcmp(kind, "facts") || !strcmp(kind, "learned_facts") ||
				 !strcmp(kind, "notes"))) ||
	       (section == 3 && !strcmp(kind, "tips")) ||
	       (section == 4 && !strcmp(kind, "past_tasks")) ||
	       (section == 5 &&
		(!strcmp(kind, "unapplied_settings") || !strcmp(kind, "unapplied_rules")));
}

static int cli_memory_section_count(const struct cli_memory_page *page, int section)
{
	cJSON *entry;
	int count = 0;

	cJSON_ArrayForEach (entry, cJSON_GetObjectItemCaseSensitive(page->report, "entries"))
		count += cli_memory_record_visible(entry, section);
	return count;
}

static const char *cli_memory_record_label(const cJSON *entry)
{
	if (cli_memory_record_visible(entry, 2))
		return "Saved fact";
	if (cli_memory_record_visible(entry, 3))
		return "Reusable tip";
	if (cli_memory_record_visible(entry, 4))
		return "Past task";
	return "Not applied";
}

static const char *cli_memory_row(void *data, size_t index)
{
	struct cli_memory_page *page = data;
	int section = (int)index;
	const char *name = cli_memory_section_name(section);

	if (section >= 2 && section <= 5)
		snprintf(page->row, sizeof(page->row), "%s (%d%s)", name,
			 cli_memory_section_count(page, section),
			 *nt_component_state_get_string(page->query) ? " matching" : "");
	else
		snprintf(page->row, sizeof(page->row), "%s", name);
	return page->row;
}

static int cli_memory_document(struct cli_memory_page *page, nb_buf *document)
{
	int section = (int)nt_list_view_selected(page->list);
	const cJSON *report = page->report;
	cJSON *entries = cJSON_GetObjectItemCaseSensitive(report, "entries");
	cJSON *entry;

	if (!section) {
		(void)nb_buf_printf(
			document,
			"Memory: %s\nProject: %s\nScope: this user, this project\n"
			"Learning from chats: %s\nSaving past work: %s\n"
			"Memory size limit: %d characters\n\n",
			cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(report, "enabled"))
				? "enabled"
				: "disabled (stored records remain inspectable)",
			runtime_workdir_borrow(page->ctx->runtime),
			cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(report, "learning_enabled"))
				? "enabled"
				: "disabled",
			cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(report, "save_tasks_enabled"))
				? "enabled"
				: "disabled",
			(int)cJSON_GetNumberValue(
				cJSON_GetObjectItemCaseSensitive(report, "reply_char_limit")));
		for (int index = 2; index <= 5; index++) {
			(void)nb_buf_printf(
				document, "%s: %d%s\n", cli_memory_section_name(index),
				cli_memory_section_count(page, index),
				*nt_component_state_get_string(page->query) ? " matching" : "");
		}
		(void)nb_buf_printf(
			document,
			"\nYour active preferences\n%s\n"
			"Saved facts and tips are inferred from past work; check them before use.\n"
			"Settings under Not applied do not change your preferences.\n\n"
			"Recent memory updates (this chat)\n%s\n"
			"Retry failed updates with /memory retry after closing this panel.\n",
			cli_memory_string(report, "preferences"),
			cli_memory_string(report, "jobs"));
		(void)nb_buf_append_cstr(document, "\nSaved project summaries\n");
		int summaries = 0;

		cJSON_ArrayForEach (entry, entries) {
			if (strcmp(cli_memory_string(entry, "kind"), "summary"))
				continue;
			summaries++;
			(void)nb_buf_printf(document, "\n%s\n%s\nSources: %s\n",
					    cli_memory_string(entry, "key"),
					    cli_memory_string(entry, "value"),
					    cli_memory_string(entry, "details"));
		}
		if (!summaries)
			(void)nb_buf_append_cstr(document, "No saved summary yet. Available notes "
							   "remain in the memory directory.\n");
	} else if (section == 1) {
		(void)nb_buf_printf(
			document,
			"Your active preferences (this chat > this project > all chats)\n%s\n"
			"Changes and sources\n%s\n"
			"Edit with /memory set or /memory unset after closing this panel.\n",
			cli_memory_string(report, "preferences"),
			cli_memory_string(report, "history"));
	} else if (section == 6) {
		(void)nb_buf_printf(
			document,
			"Recent memory updates (this chat)\n%s\n"
			"Retry failed updates with /memory retry after closing this panel.\n",
			*cli_memory_string(report, "jobs") ? cli_memory_string(report, "jobs")
							   : "No memory updates yet.\n");
	} else if (section == 7) {
		const char *context = cli_memory_string(report, "reply_memory");

		(void)nb_buf_printf(
			document,
			"Memory prepared for the most recent reply\n"
			"This view is not changed by the panel search.\n"
			"To see exactly what was sent, use /history --last-request.\n\n%s\n",
			*context ? context : "No reply memory has been prepared for this chat.");
	} else {
		int shown = 0;

		cJSON_ArrayForEach (entry, entries) {
			if (!cli_memory_record_visible(entry, section))
				continue;
			shown++;
			time_t updated = (time_t)cJSON_GetNumberValue(
				cJSON_GetObjectItemCaseSensitive(entry, "updated_at"));
			struct tm local = {0};
			char date[32] = "unknown";

			if (localtime_r(&updated, &local))
				(void)strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &local);
			(void)nb_buf_printf(document,
					    "[%s #%lld] %s\n%s\n\nSource: %s\n"
					    "Session: %s\nUpdated: %s\nDetails: %s\n\n",
					    cli_memory_record_label(entry),
					    (long long)cJSON_GetNumberValue(
						    cJSON_GetObjectItemCaseSensitive(entry, "id")),
					    cli_memory_string(entry, "key"),
					    cli_memory_string(entry, "value"),
					    cli_memory_string(entry, "source"),
					    cli_memory_string(entry, "session"), date,
					    cli_memory_string(entry, "details"));
		}
		if (!shown)
			(void)nb_buf_append_cstr(document,
						 "No matching memories in this section.\n");
	}
	return nb_buf_status(document);
}

static int cli_memory_title(void *data, struct nt_value *value)
{
	struct cli_memory_page *page = data;
	char title[96] = "morph / memory";
	if (nt_component_state_get_bool(page->detail))
		snprintf(title, sizeof(title), "morph / memory / %s",
			 cli_memory_section_name((int)nt_list_view_selected(page->list)));
	return nt_value_set_string(value, title);
}

static int cli_memory_description(void *data, struct nt_value *value)
{
	return nt_value_set_string(
		value, cli_memory_section_description((int)nt_list_view_selected(data)));
}

static int cli_memory_help(void *data, struct nt_value *value)
{
	return nt_value_set_string(
		value, nt_component_state_get_bool(data)
			       ? "Up/Down/PgUp/PgDn scroll · / search · r refresh · Esc back"
			       : "Up/Down · Enter open · / search · r refresh · Esc back");
}

static int cli_memory_status(void *data, struct nt_value *value)
{
	struct cli_memory_page *page = data;
	const char *query = nt_component_state_get_string(page->query);
	return nt_value_set_string(value, nt_component_state_get_bool(page->searching)
						  ? "Search: enter terms, then Enter; Esc cancels"
					  : *query ? query
						   : "All project memories");
}

static int cli_memory_focus(struct cli_memory_page *page)
{
	int rc = nt_view_flush_state(nt_view_controller_root_view(&page->base));
	return rc ? rc
		  : nt_view_focus(nt_component_state_get_bool(page->searching) ? page->search
				  : nt_component_state_get_bool(page->detail)  ? page->document
									       : page->list);
}

static int cli_memory_show_document(struct cli_memory_page *page)
{
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = cli_memory_document(page, &text);
	return rc ? rc : nt_text_view_set_text(page->document, nb_buf_cstr(&text));
}

static int cli_memory_refresh(struct cli_memory_page *page)
{
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc = runtime_memory_report_copy(page->ctx->runtime,
					    nt_component_state_get_string(page->query), &report);

	if (rc != 0)
		return rc;
	cJSON_Delete(page->report);
	page->report = cJSON_StealOwner(&report);
	nt_view_invalidate(page->list, 0);
	return nt_component_state_get_bool(page->detail) ? cli_memory_show_document(page) : 0;
}

static int cli_memory_action(void *data,
			     const struct nt_view_event *action)
{
	struct cli_memory_page *page = data;

	if (action->target == page->list) {
		int rc = cli_memory_show_document(page);
		if (!rc)
			rc = nt_component_state_set_bool(page->detail, true);
		return rc ? rc : cli_memory_focus(page);
	}
	int rc = nt_component_state_set_string(page->query, action->text ? action->text : "");
	if (!rc)
		rc = nt_component_state_set_bool(page->searching, false);
	if (!rc)
		rc = cli_memory_focus(page);
	return rc ? rc : cli_memory_refresh(page);
}

static void cli_memory_build(struct nt_view_controller *controller, struct nt_builder *builder)
{
	struct cli_memory_page *page = (struct cli_memory_page *)controller;
	const struct nt_view_options inset = {.padding = {1, 1, 1, 1}};
	const struct nt_view_options fill = {.grow = 1};
	const struct nt_view_options heading = cli_panel_heading(NT_AUTO);
	const struct nt_view_options hint = cli_panel_hint(NT_AUTO);
	struct nt_view *root = nt_stack_view_add(builder, NULL, NT_STACK_VIEW_VERTICAL, 0, &inset);

	page->ctx = controller->data;
	struct nt_view *title = nt_label_add(builder, root, "morph / memory", &heading);
	struct nt_view *description = nt_label_add(builder, root, "", NULL);
	page->list = cli_list_add(builder, root, &fill);
	page->document = nt_text_view_add(builder, root, "", &fill);
	struct nt_view *status = nt_label_add(builder, root, "", NULL);
	page->search = nt_text_field_add(builder, root,
					 &(struct nt_text_field_options){.limit = 2049}, NULL);
	struct nt_view *help = nt_label_add(builder, root, "", &hint);
	if (!nt_builder_error(builder)) {
		nt_builder_fail(builder, nt_label_set_wrap_mode(description, NT_TEXT_WRAP_CHAR));
		nt_builder_fail(builder,
				nt_text_view_set_wrap_mode(page->document, NT_TEXT_WRAP_CHAR));
		nt_builder_fail(builder, nt_view_use_state_string(root, "", &page->query));
		nt_builder_fail(builder, nt_view_use_state_bool(root, false, &page->detail));
		nt_builder_fail(builder, nt_view_use_state_bool(root, false, &page->searching));
		cli_bind_hidden(builder, page->list, page->detail);
		cli_bind_visible(builder, page->document, page->detail);
		cli_bind_visible(builder, page->search, page->searching);
		nt_builder_fail(builder,
				nt_view_bind_compute(title, NT_PROP_TEXT, cli_memory_title, page));
		nt_builder_fail(builder, nt_view_bind_compute(description, NT_PROP_TEXT,
							      cli_memory_description, page->list));
		nt_builder_fail(builder, nt_view_bind_compute(help, NT_PROP_TEXT, cli_memory_help,
							      page->detail));
		nt_builder_fail(builder, nt_view_bind_compute(status, NT_PROP_TEXT,
							      cli_memory_status, page));
		nt_builder_fail(builder,
				nt_list_view_set_source(page->list, 8, cli_memory_row, page));
		nt_list_view_set_navigation(page->list, 0);
		nt_list_view_set_activate_on_click(page->list, 1);
		if (!nt_view_on(page->list, NT_VIEW_EVENT_ACTIVATE, cli_memory_action, page) ||
		    !nt_view_on(page->search, NT_VIEW_EVENT_SUBMIT, cli_memory_action, page))
			nt_builder_fail(builder, NB_ERR_NOMEM);
		nt_builder_fail(builder, cli_memory_refresh(page));
		nt_builder_fail(builder, cli_memory_focus(page));
	}
}

static int cli_memory_event(struct nt_view_controller *controller, const struct nt_event *event)
{
	struct cli_memory_page *page = (struct cli_memory_page *)controller;
	int rc;

	if (event->type != NT_EVENT_KEY && event->type != NT_EVENT_TEXT)
		return 0;
	if (nt_component_state_get_bool(page->searching)) {
		if (event->key != 27)
			return 0;
		rc = nt_component_state_set_bool(page->searching, false);
	} else if ((event->key == 27 || event->key == 'q') &&
		   nt_component_state_get_bool(page->detail))
		rc = nt_component_state_set_bool(page->detail, false);
	else if (event->key == 27 || event->key == 'q' || event->key == 3) {
		rc = nt_view_controller_dismiss(controller, 0);
		return rc ? rc : 1;
	} else if (event->key == '/') {
		rc = nt_text_field_set_text(page->search,
					    nt_component_state_get_string(page->query));
		if (!rc)
			rc = nt_component_state_set_bool(page->searching, true);
	} else if (event->key == 'r') {
		rc = cli_memory_refresh(page);
		return rc ? rc : 1;
	} else
		return 0;
	if (!rc)
		rc = cli_memory_focus(page);
	return rc ? rc : 1;
}

static void cli_memory_cleanup(void *data, int result)
{
	struct cli_memory_page *page = data;

	(void)result;
	cJSON_Delete(page->report);
}

static void cli_memory_appear(struct nt_view_controller *controller)
{
	((struct cli_memory_page *)controller)->presented = 1;
}

static void cli_memory_destroy(struct nt_view_controller *controller)
{
	struct cli_memory_page *page = (struct cli_memory_page *)controller;

	int result = nt_view_controller_result(controller);
	if (page->presented)
		cli_application_panel_complete(page->ctx, page, cli_memory_cleanup, result);
	else
		cli_memory_cleanup(page, result);
}

int cli_memory_page_show(struct cli_application *ctx)
{
	static const struct nt_view_controller_class type = {
		.superclass = &nt_view_controller_base_class,
		.instance_size = sizeof(struct cli_memory_page),
		.load_view = cli_memory_build,
		.handle_event = cli_memory_event,
		.view_did_appear = cli_memory_appear,
		.deinit = cli_memory_destroy,
	};
	return cli_application_present_page(cli_application_tui_borrow(ctx), &type, ctx, 0);
}
