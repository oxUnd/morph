#include "sapi/cli/page/jobs.h"
#include <nb/err.h>
#include "runtime/runtime.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "cJSON.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/application.h"
#include "sapi/cli/component/feed.h"
#include "sapi/cli/component/feed_block.h"
#include <nb/cleanup.h>
#include "sapi/cli/component/list.h"
#include "sapi/cli/models/jobs.h"
#include "sapi/cli/component/jobs.h"
#include <nothing/tui/models/list_navigation.h>
#include <nothing/tui/controls/list_view.h>
#include <nothing/tui/views/text_view.h>
#include <nothing/tui/view_controller.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/stack_view.h>

struct cli_jobs_view {
	struct cli_application *ctx;
	cJSON *jobs;
	cJSON *details;
	struct nt_list_state selection;
	nb_buf document;
	int detail;
	int64_t refreshed;
	const char *message;
};

static int cli_jobs_refresh(struct cli_jobs_view *view)
{
	cJSON *jobs = NULL;
	int rc = runtime_process_list(view->ctx->runtime, &jobs);
	if (rc)
		return rc;
	cJSON *selected = cJSON_GetArrayItem(view->jobs, (int)view->selection.index);
	char *selected_id = selected ? strdup(cli_jobs_job_string(selected, "session_id")) : NULL;
	if (selected && !selected_id) {
		cJSON_Delete(jobs);
		return NB_ERR_NOMEM;
	}
	cJSON_Delete(view->jobs);
	view->jobs = jobs;
	if (selected_id) {
		cJSON *entry;
		size_t index = 0;
		cJSON_ArrayForEach (entry, jobs) {
			if (!strcmp(cli_jobs_job_string(entry, "session_id"), selected_id)) {
				view->selection.index = index;
				break;
			}
			index++;
		}
		free(selected_id);
	}
	view->refreshed = cli_jobs_now();
	if (view->detail) {
		cJSON *job = cJSON_GetArrayItem(jobs, (int)view->selection.index);
		cJSON *details = NULL;
		if (job &&
		    !runtime_process_read(view->ctx->runtime,
					  cli_jobs_job_string(job, "session_id"), &details)) {
			cJSON_Delete(view->details);
			view->details = details;
		}
	}
	return 0;
}

static int cli_jobs_document(struct cli_jobs_view *view)
{
	nb_buf_clear(&view->document);
	(void)nb_buf_printf(&view->document, "%s\n%s\nDirectory: %s\nStatus: %s",
			    cli_jobs_job_string(view->details, "description"),
			    cli_jobs_job_string(view->details, "command"),
			    cli_jobs_job_string(view->details, "workdir"),
			    cli_jobs_job_string(view->details, "status"));
	const cJSON *exit_code = cJSON_GetObjectItemCaseSensitive(view->details, "exit_code");
	if (cJSON_IsNumber(exit_code))
		(void)nb_buf_printf(&view->document, " · exit %d", exit_code->valueint);
	(void)nb_buf_printf(&view->document, "\n\n%s%s",
			    cli_jobs_job_string(view->details, "stdout"),
			    cli_jobs_job_string(view->details, "stderr"));
	if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(view->details, "stdout_truncated")) ||
	    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(view->details, "stderr_truncated")))
		(void)nb_buf_append_cstr(&view->document,
					 "\n[Output truncated; retained window shown]");
	return nb_buf_status(&view->document);
}

static void cli_jobs_view_cleanup(struct cli_jobs_view *view)
{
	cJSON_Delete(view->jobs);
	cJSON_Delete(view->details);
	nb_buf_cleanup(&view->document);
}

struct cli_jobs_controller {
	struct cli_jobs_view view;
	struct nt_view *title, *list, *document, *message;
	char row[BUFSIZ];
};

static const char *cli_jobs_controller_row(void *data, size_t index)
{
	struct cli_jobs_controller *page = data;
	cJSON *job = cJSON_GetArrayItem(page->view.jobs, (int)index);

	snprintf(page->row, sizeof(page->row), "%.8s  %-11s  %s", cli_jobs_job_short_id(job),
		 cli_jobs_job_string(job, "status"), cli_jobs_job_string(job, "command"));
	return page->row;
}

static int cli_jobs_controller_sync(struct cli_jobs_controller *page, int focus)
{
	struct cli_jobs_view *view = &page->view;
	size_t count = (size_t)cJSON_GetArraySize(view->jobs);
	size_t selected = view->selection.index;
	int rc = nt_list_view_set_source(page->list, count, cli_jobs_controller_row, page);
	view->selection.index = selected;

	nt_list_state_layout(&view->selection, count, view->selection.page);
	if (!rc && count)
		rc = nt_list_view_select(page->list, view->selection.index);
	if (!rc)
		rc = nt_view_set_hidden(page->list, view->detail);
	if (!rc)
		rc = nt_view_set_hidden(page->document, !view->detail);
	if (!rc)
		rc = nt_label_set_text(page->title,
				       view->detail ? "morph / jobs / output" : "morph / jobs");
	if (!rc)
		rc = nt_label_set_text(
			page->message,
			view->message ? view->message
				      : (!count ? "No background jobs in this session." : ""));
	if (!rc && view->detail && view->details) {
		rc = cli_jobs_document(view);
		if (!rc && strcmp(nt_text_view_text(page->document), nb_buf_cstr(&view->document)))
			rc = nt_text_view_set_text(page->document, nb_buf_cstr(&view->document));
	}
	if (!rc && focus)
		rc = nt_view_focus(view->detail ? page->document : page->list);
	return rc;
}

static int cli_jobs_controller_action(void *data, const struct nt_view_event *action)
{
	struct cli_jobs_controller *page = data;

	page->view.selection.index = action->index;
	if (action->type != NT_VIEW_EVENT_ACTIVATE)
		return 0;
	page->view.detail = 1;
	int rc = cli_jobs_refresh(&page->view);

	return rc ? rc : cli_jobs_controller_sync(page, 1);
}

static void cli_jobs_controller_build(struct nt_view_controller *controller,
				      struct nt_builder *builder)
{
	struct cli_jobs_controller *page = controller->data;
	const struct nt_view_options inset = {.padding = {1, 1, 1, 1}};
	const struct nt_view_options fill = {.grow = 1};
	struct nt_view *root = nt_stack_view_add(builder, NULL, NT_STACK_VIEW_VERTICAL, 0, &inset);

	page->title = nt_label_add(builder, root, "morph / jobs", NULL);
	nt_label_add(builder, root, "", NULL);
	page->list = cli_list_add(builder, root, &fill);
	page->document = nt_text_view_add(builder, root, "", &fill);
	page->message = nt_label_add(builder, root, "", NULL);
	nt_label_add(builder, root, "Up/Down · Enter output · k stop · r refresh · Esc back", NULL);
	if (!nt_builder_error(builder)) {
		nt_builder_fail(builder,
				nt_text_view_set_wrap_mode(page->document, NT_TEXT_WRAP_CHAR));
		nt_list_view_set_navigation(page->list, 0);
		nt_list_view_set_activate_on_click(page->list, 1);
		if (!nt_view_on(page->list, NT_VIEW_EVENT_ACTIVATE, cli_jobs_controller_action,
				page) ||
		    !nt_view_on(page->list, NT_VIEW_EVENT_CHANGED, cli_jobs_controller_action,
				page))
			nt_builder_fail(builder, NB_ERR_NOMEM);
		nt_builder_fail(builder, cli_jobs_controller_sync(page, 1));
	}
}

static int cli_jobs_controller_event(struct nt_view_controller *controller,
				     const struct nt_event *event)
{
	struct cli_jobs_controller *page = controller->data;
	struct cli_jobs_view *view = &page->view;
	int focus = 0;

	if (event->type == NT_EVENT_TICK) {
		if (cli_jobs_now() - view->refreshed < 1000)
			return 0;
	} else if (event->type == NT_EVENT_KEY || event->type == NT_EVENT_TEXT) {
		int key = event->type == NT_EVENT_TEXT && event->text_length == 1
				  ? (unsigned char)event->text[0]
				  : event->key;
		if ((key == 27 || key == 'q') && view->detail) {
			view->detail = 0;
			focus = 1;
		} else if (key == 27 || key == 'q' || key == 3) {
			int rc = nt_view_controller_dismiss(controller, 0);
			return rc ? rc : 1;
		} else if (key == 'k') {
			cJSON *job = cJSON_GetArrayItem(view->jobs, (int)view->selection.index);

			if (job) {
				int rc = runtime_process_stop(
					view->ctx->runtime, cli_jobs_job_string(job, "session_id"));

				view->message = rc ? nb_err_str(morph_error_descriptions(),
								MORPH_ERROR_COUNT, rc)
						   : "Stop requested";
			}
		} else if (key != 'r')
			return 0;
	} else
		return 0;
	int rc = cli_jobs_refresh(view);

	if (!rc)
		rc = cli_jobs_controller_sync(page, focus);
	return rc ? rc : 1;
}

static void cli_jobs_controller_cleanup(void *data, int result)
{
	struct cli_jobs_controller *page = data;

	(void)result;
	cli_jobs_view_cleanup(&page->view);
	free(page);
}

static void cli_jobs_controller_destroy(struct nt_view_controller *controller)
{
	struct cli_jobs_controller *page = controller->data;

	if (page)
		cli_application_panel_complete(page->view.ctx, page, cli_jobs_controller_cleanup,
					       nt_view_controller_result(controller));
}

int cli_jobs_view(struct cli_application *ctx, const char *id)
{
	static const struct nt_view_controller_class type = {
		.superclass = &nt_view_controller_base_class,
		.instance_size = sizeof(struct nt_view_controller),
		.load_view = cli_jobs_controller_build,
		.handle_event = cli_jobs_controller_event,
		.deinit = cli_jobs_controller_destroy};
	struct cli_jobs_controller *page = calloc(1, sizeof(*page));
	if (!page)
		return NB_ERR_NOMEM;
	struct cli_jobs_view *view = &page->view;
	view->ctx = ctx;
	int rc = cli_jobs_refresh(view);
	if (!rc && id) {
		cJSON *details = NULL;
		rc = runtime_process_read(ctx->runtime, id, &details);
		if (!rc) {
			cJSON *job;
			size_t index = 0;
			cJSON_ArrayForEach (job, view->jobs) {
				if (!strcmp(cli_jobs_job_string(job, "session_id"),
					    cli_jobs_job_string(details, "session_id"))) {
					view->selection.index = index;
					view->detail = 1;
					break;
				}
				index++;
			}
			if (!view->detail)
				rc = NB_ERR_NOT_FOUND;
			else
				rc = cli_jobs_refresh(view);
		}
		cJSON_Delete(details);
	}
	int accepted = 0;
	if (!rc)
		rc = cli_application_show_page(ctx, &type, page, 1000, &accepted);
	if (!accepted)
		cli_jobs_controller_cleanup(page, rc);
	return rc;
}
