#include "sapi/cli/pages/jobs.h"
#include "sapi/cli/components/list.h"
#include "sapi/cli/presentation/panel.h"
#include <nb/err.h>
#include "runtime/runtime.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "cJSON.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/feed.h"
#include "sapi/cli/presentation/feed_block.h"
#include <nb/cleanup.h>

#include "sapi/cli/models/jobs.h"
#include "sapi/cli/presentation/jobs.h"
#include <nothing/tui/controls/list_view.h>
#include <nothing/tui/views/text_view.h>
#include <nothing/tui/view_controller.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/stack_view.h>

struct cli_jobs_snapshot {
	struct cli_application *ctx;
	cJSON *jobs;
	cJSON *details;
	size_t selected;
	nb_buf document;
	int64_t refreshed;
	const char *message;
};

static int cli_jobs_refresh(struct cli_jobs_snapshot *snapshot, bool detail)
{
	cJSON *jobs = NULL;
	int rc = runtime_process_list(snapshot->ctx->runtime, &jobs);
	if (rc)
		return rc;

	cJSON *selected = cJSON_GetArrayItem(snapshot->jobs, (int)snapshot->selected);
	char *selected_id = selected ? strdup(cli_jobs_job_string(selected, "session_id")) : NULL;
	if (selected && !selected_id) {
		cJSON_Delete(jobs);
		return NB_ERR_NOMEM;
	}

	cJSON_Delete(snapshot->jobs);
	snapshot->jobs = jobs;
	if (selected_id) {
		cJSON *entry;
		size_t index = 0;
		cJSON_ArrayForEach (entry, jobs) {
			if (!strcmp(cli_jobs_job_string(entry, "session_id"), selected_id)) {
				snapshot->selected = index;
				break;
			}

			index++;
		}

		free(selected_id);
	}

	snapshot->refreshed = cli_jobs_now();
	if (detail) {
		cJSON *job = cJSON_GetArrayItem(jobs, (int)snapshot->selected);
		cJSON *details = NULL;
		if (job &&
		    !runtime_process_read(snapshot->ctx->runtime,
					  cli_jobs_job_string(job, "session_id"), &details)) {
			cJSON_Delete(snapshot->details);
			snapshot->details = details;
		}
	}

	return 0;
}

static int cli_jobs_document(struct cli_jobs_snapshot *snapshot)
{
	nb_buf_clear(&snapshot->document);
	(void)nb_buf_printf(&snapshot->document, "%s\n%s\nDirectory: %s\nStatus: %s",
			    cli_jobs_job_string(snapshot->details, "description"),
			    cli_jobs_job_string(snapshot->details, "command"),
			    cli_jobs_job_string(snapshot->details, "workdir"),
			    cli_jobs_job_string(snapshot->details, "status"));
	const cJSON *exit_code = cJSON_GetObjectItemCaseSensitive(snapshot->details, "exit_code");
	if (cJSON_IsNumber(exit_code))
		(void)nb_buf_printf(&snapshot->document, " · exit %d", exit_code->valueint);

	(void)nb_buf_printf(&snapshot->document, "\n\n%s%s",
			    cli_jobs_job_string(snapshot->details, "stdout"),
			    cli_jobs_job_string(snapshot->details, "stderr"));
	if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(snapshot->details, "stdout_truncated")) ||
	    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(snapshot->details, "stderr_truncated")))
		(void)nb_buf_append_cstr(&snapshot->document,
					 "\n[Output truncated; retained window shown]");

	return nb_buf_status(&snapshot->document);
}

struct cli_jobs_view {
	struct nt_stack_view base;
	struct cli_jobs_snapshot *snapshot;
	struct nt_component_state *detail;
	struct nt_view *list, *document, *message;
	char row[BUFSIZ];
};

static const char *cli_jobs_row(void *data, size_t index)
{
	struct cli_jobs_view *view = data;
	cJSON *job = cJSON_GetArrayItem(view->snapshot->jobs, (int)index);

	snprintf(view->row, sizeof(view->row), "%.8s  %-11s  %s", cli_jobs_job_short_id(job),
		 cli_jobs_job_string(job, "status"), cli_jobs_job_string(job, "command"));
	return view->row;
}

static int cli_jobs_title(void *data, struct nt_value *value)
{
	return nt_value_set_string(value, nt_component_state_get_bool(data)
						  ? "morph / jobs / output"
						  : "morph / jobs");
}

static int cli_jobs_show_snapshot(struct cli_jobs_view *view)
{
	struct cli_jobs_snapshot *snapshot = view->snapshot;
	size_t count = (size_t)cJSON_GetArraySize(snapshot->jobs);
	size_t selected = snapshot->selected < count ? snapshot->selected : count ? count - 1 : 0;
	int rc = nt_list_view_set_source(view->list, count, cli_jobs_row, view);
	if (!rc && count)
		rc = nt_list_view_select(view->list, selected);

	snapshot->selected = nt_list_view_selected(view->list);
	if (!rc)
		rc = nt_label_set_text(
			view->message,
			snapshot->message ? snapshot->message
				      : (!count ? "No background jobs in this session." : ""));

	if (!rc && nt_component_state_get_bool(view->detail) && snapshot->details) {
		rc = cli_jobs_document(snapshot);
		if (!rc && strcmp(nt_text_view_text(view->document), nb_buf_cstr(&snapshot->document)))
			rc = nt_text_view_set_text(view->document, nb_buf_cstr(&snapshot->document));
	}

	return rc;
}

static int cli_jobs_focus(struct cli_jobs_view *view)
{
	int rc = nt_view_flush_state(&view->base.base);
	return rc ? rc
		  : nt_view_focus(nt_component_state_get_bool(view->detail) ? view->document
									    : view->list);
}

static int cli_jobs_controller_action(void *data, const struct nt_view_event *action)
{
	struct cli_jobs_view *view = data;

	view->snapshot->selected = action->index;
	if (action->type != NT_VIEW_EVENT_ACTIVATE)
		return 0;

	int rc = cli_jobs_refresh(view->snapshot, true);
	if (!rc)
		rc = nt_component_state_set_bool(view->detail, true);

	if (!rc)
		rc = cli_jobs_focus(view);

	return rc ? rc : cli_jobs_show_snapshot(view);
}

static void cli_jobs_controller_build(struct nt_view_controller *controller,
				      struct nt_builder *builder)
{
	static const struct nt_view_class type = {.superclass = &nt_stack_view_class,
						  .instance_size = sizeof(struct cli_jobs_view),
						  .kind = "cli_jobs"};
	const struct nt_view_options inset = {.padding = {1, 1, 1, 1}};
	const struct nt_view_options fill = {.grow = 1};
	const struct nt_view_options heading = cli_panel_heading(NT_AUTO);
	const struct nt_view_options hint = cli_panel_hint(NT_AUTO);
	struct nt_view *root = nt_view_add(builder, NULL, &type, &inset);
	if (!root)
		return;

	struct cli_jobs_view *view = (struct cli_jobs_view *)root;
	view->snapshot = controller->data;
	nt_builder_fail(builder, nt_view_use_state_bool(root, view->snapshot->details != NULL,
							&view->detail));
	struct nt_view *title = nt_label_add(builder, root, "morph / jobs", &heading);
	nt_label_add(builder, root, "", NULL);
	view->list = cli_list_add(builder, root, &fill);
	view->document = nt_text_view_add(builder, root, "", &fill);
	view->message = nt_label_add(builder, root, "", NULL);
	nt_label_add(builder, root, "Up/Down · Enter output · k stop · r refresh · Esc back",
		     &hint);
	if (!nt_builder_error(builder)) {
		nt_builder_fail(builder,
				nt_text_view_set_wrap_mode(view->document, NT_TEXT_WRAP_CHAR));
		cli_panel_hide_when(builder, view->list, view->detail);
		cli_panel_show_when(builder, view->document, view->detail);
		nt_builder_fail(builder, nt_view_bind_compute(title, NT_PROP_TEXT, cli_jobs_title,
							      view->detail));
		nt_list_view_set_navigation(view->list, 0);
		nt_list_view_set_activate_on_click(view->list, 1);
		if (!nt_view_on(view->list, NT_VIEW_EVENT_ACTIVATE, cli_jobs_controller_action,
				view) ||
		    !nt_view_on(view->list, NT_VIEW_EVENT_CHANGED, cli_jobs_controller_action,
				view))
			nt_builder_fail(builder, NB_ERR_NOMEM);

		nt_builder_fail(builder, cli_jobs_show_snapshot(view));
		nt_builder_fail(builder, cli_jobs_focus(view));
	}
}

static int cli_jobs_controller_event(struct nt_view_controller *controller,
				     const struct nt_event *event)
{
	struct cli_jobs_view *view =
		(struct cli_jobs_view *)nt_view_controller_root_view(controller);
	struct cli_jobs_snapshot *snapshot = view->snapshot;
	bool detail = nt_component_state_get_bool(view->detail);

	if (event->type == NT_EVENT_TICK) {
		if (cli_jobs_now() - snapshot->refreshed < 1000)
			return 0;
	} else if (event->type == NT_EVENT_KEY || event->type == NT_EVENT_TEXT) {
		int key = event->type == NT_EVENT_TEXT && event->text_length == 1
				  ? (unsigned char)event->text[0]
				  : event->key;
		if ((key == 27 || key == 'q') && detail) {
			int rc = nt_component_state_set_bool(view->detail, false);
			if (!rc)
				rc = cli_jobs_focus(view);

			return rc ? rc : 1;
		} else if (key == 27 || key == 'q' || key == 3) {
			int rc = nt_view_controller_dismiss(controller, 0);
			return rc ? rc : 1;
		} else if (key == 'k') {
			cJSON *job = cJSON_GetArrayItem(snapshot->jobs, (int)snapshot->selected);

			if (job) {
				int rc = runtime_process_stop(
					snapshot->ctx->runtime, cli_jobs_job_string(job, "session_id"));

				snapshot->message = rc ? nb_err_str(morph_error_descriptions(),
								MORPH_ERROR_COUNT, rc)
						   : "Stop requested";
			}
		} else if (key != 'r')
			return 0;
	} else
		return 0;

	int rc = cli_jobs_refresh(snapshot, detail);
	if (!rc)
		rc = cli_jobs_show_snapshot(view);

	return rc ? rc : 1;
}

static void cli_jobs_controller_cleanup(void *data, int result)
{
	struct cli_jobs_snapshot *snapshot = data;
	(void)result;
	cJSON_Delete(snapshot->jobs);
	cJSON_Delete(snapshot->details);
	nb_buf_cleanup(&snapshot->document);
	free(snapshot);
}

static void cli_jobs_controller_destroy(struct nt_view_controller *controller)
{
	struct cli_jobs_snapshot *snapshot = controller->data;

	if (snapshot)
		cli_application_panel_complete(snapshot->ctx, snapshot, cli_jobs_controller_cleanup,
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
	struct cli_jobs_snapshot *snapshot = calloc(1, sizeof(*snapshot));
	if (!snapshot)
		return NB_ERR_NOMEM;

	snapshot->ctx = ctx;
	int rc = cli_jobs_refresh(snapshot, false);
	if (!rc && id) {
		cJSON *details = NULL;
		rc = runtime_process_read(ctx->runtime, id, &details);
		if (!rc) {
			cJSON *job;
			size_t index = 0;
			cJSON_ArrayForEach (job, snapshot->jobs) {
				if (!strcmp(cli_jobs_job_string(job, "session_id"),
					    cli_jobs_job_string(details, "session_id"))) {
					snapshot->selected = index;
					snapshot->details = cJSON_StealOwner(&details);
					break;
				}

				index++;
			}

			if (!snapshot->details)
				rc = NB_ERR_NOT_FOUND;
		}

		cJSON_Delete(details);
	}

	int accepted = 0;
	if (!rc)
		rc = cli_application_show_page(ctx, &type, snapshot, 1000, &accepted);

	if (!accepted)
		cli_jobs_controller_cleanup(snapshot, rc);

	return rc;
}
