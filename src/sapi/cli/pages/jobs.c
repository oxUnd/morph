#include "sapi/cli/pages/jobs.h"
#include "errors.h"
#include "sapi/cli/components/list.h"
#include "sapi/cli/components/page_layout.h"
#include "sapi/cli/presentation/panel.h"
#include <nb/err.h>
#include "runtime/runtime.h"
#include "cJSON.h"
#include "sapi/cli/application.h"
#include <nb/cleanup.h>
#include <string.h>
#include <stdio.h>
#include "sapi/cli/models/jobs.h"
#include "sapi/cli/services/processes.h"
#include <nothing/tui/controls/list_view.h>
#include <nothing/tui/views/text_view.h>
#include <nothing/tui/view_controller.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/stack_view.h>

struct cli_jobs_page {
	struct nt_view_controller base;
	struct cli_page_layout layout;
	cJSON *jobs, *details;
	size_t selected;
	int64_t refreshed;
	const char *status;
	struct nt_component_state *detail;
	struct nt_view *list, *document, *message;
	char row[BUFSIZ];
};

static int cli_jobs_refresh(struct cli_jobs_page *page, struct cli_application *ctx, bool detail)
{
	cJSON *jobs = NULL;
	int rc = cli_process_list(ctx, &jobs);
	if (rc)
		return rc;

	cJSON *selected = cJSON_GetArrayItem(page->jobs, (int)page->selected);
	if (selected) {
		const char *id = cli_jobs_job_string(selected, "session_id");
		cJSON *entry;
		size_t index = 0;
		cJSON_ArrayForEach (entry, jobs) {
			if (!strcmp(cli_jobs_job_string(entry, "session_id"), id)) {
				page->selected = index;
				break;
			}

			index++;
		}
	}

	cJSON_Delete(page->jobs);
	page->jobs = jobs;
	page->refreshed = cli_jobs_now();
	if (detail) {
		cJSON *job = cJSON_GetArrayItem(jobs, (int)page->selected);
		cJSON *details = NULL;
		if (job && !cli_process_read(ctx, cli_jobs_job_string(job, "session_id"),
						 &details)) {
			cJSON_Delete(page->details);
			page->details = details;
		}
	}

	return 0;
}

static int cli_jobs_document(struct cli_jobs_page *page, nb_buf *text)
{
	(void)nb_buf_printf(text, "%s\n%s\nDirectory: %s\nStatus: %s",
			    cli_jobs_job_string(page->details, "description"),
			    cli_jobs_job_string(page->details, "command"),
			    cli_jobs_job_string(page->details, "workdir"),
			    cli_jobs_job_string(page->details, "status"));
	const cJSON *exit_code = cJSON_GetObjectItemCaseSensitive(page->details, "exit_code");
	if (cJSON_IsNumber(exit_code))
		(void)nb_buf_printf(text, " · exit %d", exit_code->valueint);

	(void)nb_buf_printf(text, "\n\n%s%s", cli_jobs_job_string(page->details, "stdout"),
			    cli_jobs_job_string(page->details, "stderr"));
	if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(page->details, "stdout_truncated")) ||
	    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(page->details, "stderr_truncated")))
		(void)nb_buf_append_cstr(text, "\n[Output truncated; retained window shown]");

	return nb_buf_status(text);
}

static const char *cli_jobs_row(void *data, size_t index)
{
	struct cli_jobs_page *page = data;
	cJSON *job = cJSON_GetArrayItem(page->jobs, (int)index);

	snprintf(page->row, sizeof(page->row), "%.8s  %-11s  %s", cli_jobs_job_short_id(job),
		 cli_jobs_job_string(job, "status"), cli_jobs_job_string(job, "command"));
	return page->row;
}

static int cli_jobs_title(void *data, struct nt_value *value)
{
	return nt_value_set_string(value, nt_component_state_get_bool(data)
						  ? "output"
						  : "");
}

static int cli_jobs_show_snapshot(struct cli_jobs_page *page)
{
	size_t count = (size_t)cJSON_GetArraySize(page->jobs);
	size_t selected = page->selected < count ? page->selected : count ? count - 1 : 0;
	int rc = nt_list_view_set_source(page->list, count, cli_jobs_row, page);
	if (!rc && count)
		rc = nt_list_view_select(page->list, selected);

	page->selected = nt_list_view_selected(page->list);
	if (!rc)
		rc = nt_label_set_text(
			page->message,
			page->status ? page->status
				     : (!count ? "No background jobs in this session." : ""));

	if (!rc && nt_component_state_get_bool(page->detail) && page->details) {
		nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
		rc = cli_jobs_document(page, &text);
		if (!rc && strcmp(nt_text_view_text(page->document), nb_buf_cstr(&text)))
			rc = nt_text_view_set_text(page->document, nb_buf_cstr(&text));
	}

	return rc;
}

static int cli_jobs_focus(struct cli_jobs_page *page)
{
	int rc = nt_view_flush_state(nt_view_controller_root_view(&page->base));
	return rc ? rc
		  : nt_view_focus(nt_component_state_get_bool(page->detail) ? page->document
									    : page->list);
}

static int cli_jobs_controller_action(void *data, const struct nt_view_event *action)
{
	struct cli_jobs_page *page = data;

	page->selected = action->index;
	if (action->type != NT_VIEW_EVENT_ACTIVATE)
		return 0;

	int rc =
		cli_jobs_refresh(page, cli_application_from_controller(&page->base), true);
	if (!rc)
		rc = nt_component_state_set_bool(page->detail, true);

	if (!rc)
		rc = cli_jobs_focus(page);

	return rc ? rc : cli_jobs_show_snapshot(page);
}

static int cli_jobs_back(void *data)
{
	struct cli_jobs_page *page = data;
	int rc = nt_component_state_set_bool(page->detail, false);
	return rc ? rc : cli_jobs_focus(page);
}

static void cli_jobs_controller_build(struct nt_view_controller *controller,
				      struct nt_builder *builder)
{
	const struct nt_view_options fill = {.grow = 1};
	const struct nt_view_options hint = cli_panel_hint(NT_AUTO);
	struct cli_jobs_page *page = (struct cli_jobs_page *)controller;
	cli_page_layout_build(builder, &page->layout, "jobs", &nt_stack_view_class,
			      cli_jobs_back, page);
	struct nt_view *root = page->layout.body;
	if (!root)
		return;

	nt_builder_fail(builder,
			nt_view_use_state_bool(root, page->details != NULL, &page->detail));
	nt_label_add(builder, root, "Background jobs", NULL);
	page->list = cli_list_add(builder, root, &fill);
	page->document = nt_text_view_add(builder, root, "", &fill);
	page->message = nt_label_add(builder, root, "", NULL);
	nt_label_add(builder, page->layout.footer,
		     "Up/Down · Enter output · k stop · r refresh · Esc back", &hint);
	if (!nt_builder_error(builder)) {
		nt_builder_fail(builder,
				nt_text_view_set_wrap_mode(page->document, NT_TEXT_WRAP_CHAR));
		cli_panel_hide_when(builder, page->list, page->detail);
		cli_panel_show_when(builder, page->document, page->detail);
		cli_page_layout_bind_subpage(builder, &page->layout, cli_jobs_title, page->detail);
		nt_list_view_set_navigation(page->list, 0);
		nt_list_view_set_activate_on_click(page->list, 1);
		if (!nt_view_on(page->list, NT_VIEW_EVENT_ACTIVATE, cli_jobs_controller_action,
				page) ||
		    !nt_view_on(page->list, NT_VIEW_EVENT_CHANGED, cli_jobs_controller_action,
				page))
			nt_builder_fail(builder, NB_ERR_NOMEM);

		nt_builder_fail(builder, cli_jobs_show_snapshot(page));
		nt_builder_fail(builder, cli_jobs_focus(page));
	}
}

static int cli_jobs_controller_event(struct nt_view_controller *controller,
				     const struct nt_event *event)
{
	struct cli_jobs_page *page = (struct cli_jobs_page *)controller;
	struct cli_application *ctx = cli_application_from_controller(controller);
	bool detail = nt_component_state_get_bool(page->detail);

	if (event->type == NT_EVENT_TICK) {
		if (cli_jobs_now() - page->refreshed < 1000)
			return 0;
	} else if (event->type == NT_EVENT_KEY || event->type == NT_EVENT_TEXT) {
		int key = event->type == NT_EVENT_TEXT && event->text_length == 1
				  ? (unsigned char)event->text[0]
				  : event->key;
		if ((key == 27 || key == 'q') && detail) {
			int rc = nt_component_state_set_bool(page->detail, false);
			if (!rc)
				rc = cli_jobs_focus(page);

			return rc ? rc : 1;
		} else if (key == 27 || key == 'q' || key == 3) {
			int rc = nt_view_controller_dismiss(controller, 0);
			return rc ? rc : 1;
		} else if (key == 'k') {
			cJSON *job = cJSON_GetArrayItem(page->jobs, (int)page->selected);

			if (job) {
				int rc = cli_process_stop(ctx,
					cli_jobs_job_string(job, "session_id"));

				page->status = rc ? nb_err_str(morph_error_descriptions(),
							       MORPH_ERROR_COUNT, rc)
						  : "Stop requested";
			}
		} else if (key != 'r')
			return 0;
	} else
		return 0;

	int rc = cli_jobs_refresh(page, ctx, detail);
	if (!rc)
		rc = cli_jobs_show_snapshot(page);

	return rc ? rc : 1;
}

static void cli_jobs_controller_destroy(struct nt_view_controller *controller)
{
	struct cli_jobs_page *page = (struct cli_jobs_page *)controller;
	cJSON_Delete(page->jobs);
	cJSON_Delete(page->details);
}

int cli_jobs_view(struct cli_application *ctx, const char *id)
{
	static const struct nt_view_controller_class type = {
		.superclass = &cli_application_panel_class,
		.instance_size = sizeof(struct cli_jobs_page),
		.load_view = cli_jobs_controller_build,
		.handle_event = cli_jobs_controller_event,
		.deinit = cli_jobs_controller_destroy};
	struct nt_view_controller *owner NB_CLEANUP(cli_application_controller_cleanup) = NULL;
	int rc = nt_view_controller_create(&type, &owner);
	if (rc)
		return rc;

	struct cli_jobs_page *page = (struct cli_jobs_page *)owner;
	rc = cli_jobs_refresh(page, ctx, false);
	if (!rc && id) {
		cJSON *details = NULL;
		rc = cli_process_read(ctx, id, &details);
		if (!rc) {
			cJSON *job;
			size_t index = 0;
			cJSON_ArrayForEach (job, page->jobs) {
				if (!strcmp(cli_jobs_job_string(job, "session_id"),
					    cli_jobs_job_string(details, "session_id"))) {
					page->selected = index;
					page->details = cJSON_StealOwner(&details);
					break;
				}

				index++;
			}

			if (!page->details)
				rc = NB_ERR_NOT_FOUND;
		}

		cJSON_Delete(details);
	}

	return rc ? rc : cli_application_show_page(ctx, &owner, 1000);
}
