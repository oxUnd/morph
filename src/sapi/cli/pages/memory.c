#include "sapi/cli/pages/memory.h"

#include "sapi/cli/application.h"
#include "runtime/services.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "cJSON.h"
#include "cJSON.h"
#include <morph/tui/view_controller.h>
#include <morph/tui/widgets/label.h>
#include <morph/tui/widgets/list.h>
#include <morph/tui/widgets/stack.h>
#include <morph/tui/widgets/text_field.h>
#include <morph/tui/widgets/text_view.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct cli_memory_page {
	struct morph_tui_view_controller base;
	struct cli_application *ctx;
	struct morph_tui_view *title;
	struct morph_tui_view *description;
	struct morph_tui_view *list;
	struct morph_tui_view *document;
	struct morph_tui_view *search;
	struct morph_tui_view *status;
	struct morph_tui_view *help;
	cJSON *report;
	char *query;
	char row[BUFSIZ];
	int section;
	int detail;
	int searching;
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
			 page->query && *page->query ? " matching" : "");
	else
		snprintf(page->row, sizeof(page->row), "%s", name);
	return page->row;
}

static int cli_memory_document(struct cli_memory_page *page, nb_buf *document)
{
	const cJSON *report = page->report;
	cJSON *entries = cJSON_GetObjectItemCaseSensitive(report, "entries");
	cJSON *entry;

	if (!page->section) {
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
		for (int section = 2; section <= 5; section++) {
			(void)nb_buf_printf(document, "%s: %d%s\n",
					    cli_memory_section_name(section),
					    cli_memory_section_count(page, section),
					    page->query && *page->query ? " matching" : "");
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
	} else if (page->section == 1) {
		(void)nb_buf_printf(
			document,
			"Your active preferences (this chat > this project > all chats)\n%s\n"
			"Changes and sources\n%s\n"
			"Edit with /memory set or /memory unset after closing this panel.\n",
			cli_memory_string(report, "preferences"),
			cli_memory_string(report, "history"));
	} else if (page->section == 6) {
		(void)nb_buf_printf(
			document,
			"Recent memory updates (this chat)\n%s\n"
			"Retry failed updates with /memory retry after closing this panel.\n",
			*cli_memory_string(report, "jobs") ? cli_memory_string(report, "jobs")
							   : "No memory updates yet.\n");
	} else if (page->section == 7) {
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
			if (!cli_memory_record_visible(entry, page->section))
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

static int cli_memory_sync(struct cli_memory_page *page)
{
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&text), nb_buf_reserve(&text, BUFSIZ) ? 0 : NB_ERR_NOMEM);

	if (rc == 0)
		rc = (nb_buf_init(&title), nb_buf_reserve(&title, 128) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = page->detail
			     ? (nb_buf_printf(&title, "morph / memory / %s",
					      cli_memory_section_name(page->section))
					? 0
					: NB_ERR_NOMEM)
			     : (nb_buf_append_cstr(&title, "morph / memory") ? 0 : NB_ERR_NOMEM);
	if (rc == 0 && page->detail)
		rc = cli_memory_document(page, &text);
	if (rc == 0)
		rc = morph_tui_widget_list_set_source_borrow(page->list, 8, cli_memory_row, page);
	if (rc == 0)
		rc = morph_tui_widget_list_select(page->list, (size_t)page->section);
	if (rc == 0)
		rc = morph_tui_view_set_hidden(page->list, page->detail);
	if (rc == 0)
		rc = morph_tui_view_set_hidden(page->document, !page->detail);
	if (rc == 0)
		rc = morph_tui_widget_label_set_text_copy(page->title, nb_buf_cstr(&title));
	if (rc == 0)
		rc = morph_tui_widget_label_set_text_copy(
			page->description, cli_memory_section_description(page->section));
	if (rc == 0 && page->detail)
		rc = morph_tui_widget_text_view_set_text_copy(page->document, nb_buf_cstr(&text));
	if (rc == 0)
		rc = morph_tui_widget_label_set_text_copy(
			page->status, page->searching
					      ? "Search: enter terms, then Enter; Esc cancels"
				      : page->query && *page->query ? page->query
								    : "All project memories");
	if (rc == 0)
		rc = morph_tui_view_set_hidden(page->search, !page->searching);
	if (rc == 0)
		rc = morph_tui_widget_label_set_text_copy(
			page->help,
			page->detail ? "Up/Down/PgUp/PgDn scroll · / search · r refresh · Esc back"
				     : "Up/Down · Enter open · / search · r refresh · Esc back");
	if (rc == 0)
		rc = morph_tui_view_focus(page->searching ? page->search
					  : page->detail  ? page->document
							  : page->list);
	return rc;
}

static int cli_memory_refresh(struct cli_memory_page *page)
{
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc = runtime_memory_report_copy(page->ctx->runtime, page->query, &report);

	if (rc != 0)
		return rc;
	cJSON_Delete(page->report);
	page->report = cJSON_StealOwner(&report);
	return cli_memory_sync(page);
}

static void cli_memory_build(struct morph_tui_view_controller *controller,
			     struct morph_tui_builder *builder)
{
	struct cli_memory_page *page = (struct cli_memory_page *)controller;
	const struct morph_tui_view_options inset = {.padding = {1, 1, 1, 1}};
	const struct morph_tui_view_options fill = {.grow = 1};
	struct morph_tui_view *root = morph_tui_widget_stack_add_borrow(
		builder, NULL, MORPH_TUI_WIDGET_STACK_VERTICAL, 0, &inset);

	page->ctx = controller->data;
	page->title = morph_tui_widget_label_add_borrow(builder, root, "morph / memory", NULL);
	page->description = morph_tui_widget_label_add_borrow(builder, root, "", NULL);
	page->list = morph_tui_widget_list_add_borrow(builder, root, NULL, 0, 0, &fill);
	page->document = morph_tui_widget_text_view_add_borrow(builder, root, "", &fill);
	page->status = morph_tui_widget_label_add_borrow(builder, root, "", NULL);
	page->search = morph_tui_widget_text_field_add_borrow(builder, root, "", 2049, 0, NULL);
	page->help = morph_tui_widget_label_add_borrow(builder, root, "", NULL);
	if (!morph_tui_builder_error(builder)) {
		morph_tui_widget_list_set_navigation(page->list, 0);
		morph_tui_builder_fail(builder, cli_memory_refresh(page));
	}
}

static int cli_memory_action(struct morph_tui_view_controller *controller,
			     const struct morph_tui_widget_action *action)
{
	struct cli_memory_page *page = (struct cli_memory_page *)controller;

	if (action->sender == page->list) {
		page->section = (int)action->index;
		if (action->type != MORPH_TUI_WIDGET_ACTION_ACTIVATE)
			return morph_tui_widget_label_set_text_copy(
				page->description, cli_memory_section_description(page->section));
		page->detail = 1;
		return cli_memory_sync(page);
	}
	if (action->sender != page->search || action->type != MORPH_TUI_WIDGET_ACTION_SUBMIT)
		return 0;
	char *query NB_CLEANUP(nb_charp_cleanup) = strdup(action->text ? action->text : "");

	if (!query)
		return NB_ERR_NOMEM;
	free(page->query);
	page->query = nb_charp_steal(&query);
	page->searching = 0;
	return cli_memory_refresh(page);
}

static int cli_memory_event(struct morph_tui_view_controller *controller,
			    const struct morph_tui_event *event)
{
	struct cli_memory_page *page = (struct cli_memory_page *)controller;
	int rc;

	if (event->type != MORPH_TUI_EVENT_KEY && event->type != MORPH_TUI_EVENT_TEXT)
		return 0;
	if (page->searching) {
		if (event->key != 27)
			return 0;
		page->searching = 0;
		rc = cli_memory_sync(page);
	} else if ((event->key == 27 || event->key == 'q') && page->detail) {
		page->detail = 0;
		rc = cli_memory_sync(page);
	} else if (event->key == '/') {
		page->searching = 1;
		rc = morph_tui_widget_text_field_set_text_copy(page->search,
							       page->query ? page->query : "");
		if (rc == 0)
			rc = cli_memory_sync(page);
	} else if (event->key == 'r')
		rc = cli_memory_refresh(page);
	else
		return 0;
	return rc ? rc : 1;
}

static void cli_memory_cleanup(void *data, int result)
{
	struct cli_memory_page *page = data;

	(void)result;
	cJSON_Delete(page->report);
	free(page->query);
}

static int cli_memory_appear(struct morph_tui_view_controller *controller)
{
	((struct cli_memory_page *)controller)->presented = 1;
	return 0;
}

static void cli_memory_destroy(struct morph_tui_view_controller *controller, int result)
{
	struct cli_memory_page *page = (struct cli_memory_page *)controller;

	if (page->presented)
		cli_application_panel_complete(page->ctx, page, cli_memory_cleanup, result);
	else
		cli_memory_cleanup(page, result);
}

int cli_memory_page_show(struct cli_application *ctx)
{
	static const struct morph_tui_view_controller_ops ops = {
		.build = cli_memory_build,
		.action = cli_memory_action,
		.event = cli_memory_event,
		.appear = cli_memory_appear,
		.destroy = cli_memory_destroy,
	};
	struct cli_memory_page page;
	const struct morph_tui_view_controller_spec spec = {
		.ops = &ops,
		.data = ctx,
		.size = sizeof(page),
		.quit_key = 'q',
	};

	return morph_tui_view_controller_present_borrow(cli_application_tui_borrow(ctx), &spec);
}
