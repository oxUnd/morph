#include "sapi/cli/pages/jobs.h"
#include "sapi/cli/presentation/output.h"
#include "util/json.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/application.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/list.h"
#include <morph/tui/components.h>
#include <morph/tui/view_controller.h>
#include <morph/tui/widgets/label.h>
#include <morph/tui/widgets/stack.h>

#define CLI_JOBS_JOB_SUCCESS_MS 5000
#define CLI_JOBS_JOB_FAILURE_MS 10000

struct cli_jobs_job {
	struct json *data;
	int64_t updated;
	int seen;
};

struct cli_jobs {
	morph_array_t entries;
};

static int64_t cli_jobs_now(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return 0;
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static const char *cli_jobs_job_short_id(const struct json *data)
{
	const char *id = json_str_or(json_member(data, "session_id"), NULL, "");
	return !strncmp(id, "proc_", 5) ? id + 5 : id;
}

static int cli_jobs_job_running(const struct json *data)
{
	const char *state = json_str_or(json_member(data, "status"), NULL, "");
	return !strcmp(state, "running") || !strcmp(state, "starting");
}

static int cli_jobs_job_failed(const struct json *data)
{
	return !cli_jobs_job_running(data) &&
	       (strcmp(json_str_or(json_member(data, "status"), NULL, ""), "exited") ||
		json_number_value(json_member(data, "exit_code")) != 0);
}

static int cli_jobs_job_visible(const struct cli_application *ctx, const struct cli_jobs_job *job)
{
	int64_t owner = 0;
	(void)runtime_session_current_id(ctx->runtime, &owner);
	if (json_number_value(json_member(job->data, "owner_session_id")) != (double)owner)
		return 0;
	int64_t retention =
		cli_jobs_job_failed(job->data) ? CLI_JOBS_JOB_FAILURE_MS : CLI_JOBS_JOB_SUCCESS_MS;
	return cli_jobs_job_running(job->data) ||
	       (!job->seen && cli_jobs_now() - job->updated < retention);
}

static int cli_jobs_remember_job(struct cli_application *ctx, const struct json *data)
{
	if (!ctx->jobs) {
		struct cli_jobs_job entry;
		ctx->jobs = calloc(1, sizeof(*ctx->jobs));
		if (!ctx->jobs)
			return -ENOMEM;
		int rc = morph_array_init(&ctx->jobs->entries, 8, sizeof(entry));
		if (rc) {
			free(ctx->jobs);
			ctx->jobs = NULL;
			return rc;
		}
	}
	struct json *copy = json_clone(data, 1);
	if (!copy)
		return -ENOMEM;
	struct cli_jobs_job *job;
	morph_array_foreach (job, &ctx->jobs->entries, struct cli_jobs_job) {
		if (!strcmp(json_str_or(json_member(job->data, "session_id"), NULL, ""),
			    json_str_or(json_member(data, "session_id"), NULL, ""))) {
			json_free(job->data);
			job->data = copy;
			job->updated = cli_jobs_now();
			return 0;
		}
	}
	job = morph_array_push(&ctx->jobs->entries);
	if (!job) {
		json_free(copy);
		return -ENOMEM;
	}
	*job = (struct cli_jobs_job){.data = copy, .updated = cli_jobs_now()};
	return 0;
}

void cli_jobs_cleanup(struct cli_application *ctx)
{
	if (!ctx->jobs)
		return;
	struct cli_jobs_job *job;
	morph_array_foreach (job, &ctx->jobs->entries, struct cli_jobs_job)
		json_free(job->data);
	morph_array_cleanup(&ctx->jobs->entries);
	free(ctx->jobs);
	ctx->jobs = NULL;
}

int cli_jobs_event(struct cli_application *ctx, const struct morph_event *event)
{
	if (!event->name ||
	    (strcmp(event->name, "process.started") && strcmp(event->name, "process.completed")))
		return 0;
	int rc = cli_jobs_remember_job(ctx, event->data);
	if (rc)
		return rc;
	if (!strcmp(event->name, "process.completed")) {
		int64_t owner = 0;
		(void)runtime_session_current_id(ctx->runtime, &owner);
		if (json_number_value(json_member(event->data, "owner_session_id")) ==
		    (double)owner) {
			const char *description =
				json_str_or(json_member(event->data, "description"), NULL, "");
			char safe[BUFSIZ];
			(void)cli_list_compact_text(
				safe, sizeof(safe),
				description[0] ? description
					       : json_str_or(json_member(event->data, "command"),
							     NULL, ""));
			cli_feed_history_begin(ctx);
			printf(ctx, "  %s %s · %s", cli_jobs_job_failed(event->data) ? "✗" : "✓",
			       safe, json_str_or(json_member(event->data, "status"), NULL, ""));
			const struct json *exit_code = json_member(event->data, "exit_code");
			if (json_is_number_value(exit_code))
				printf(ctx, " %d", json_int_value(exit_code));
			printf(ctx, " · /jobs %.8s\n", cli_jobs_job_short_id(event->data));
			cli_feed_history_end(ctx);
		}
	}
	cli_feed_queue_changed(ctx);
	return 1;
}

int cli_jobs_visible(const struct cli_application *ctx)
{
	if (!ctx->jobs)
		return 0;
	struct cli_jobs_job *job;
	morph_array_foreach (job, &ctx->jobs->entries, struct cli_jobs_job) {
		if (cli_jobs_job_visible(ctx, job))
			return 1;
	}
	return 0;
}

static int cli_jobs_job_row(morph_buf_t *out, const char *prefix, const char *text,
			    const char *meta, int columns, const char *style)
{
	char compact[BUFSIZ], clipped[BUFSIZ];
	cli_list_compact_text(compact, sizeof(compact), text);
	int budget = columns - 1 - (int)utf8_display_width(prefix) - (int)utf8_display_width(meta) -
		     (meta[0] ? 2 : 0);
	if (budget < 1)
		budget = 1;
	utf8_copy_ellipsized_display_width(clipped, sizeof(clipped), compact, (size_t)budget, 0);
	if (out->len)
		(void)morph_buf_putc(out, '\n');
	int padding = budget - (int)utf8_display_width(clipped);
	return morph_buf_printf(out, "%s%s%s%*s%s%s", style, prefix, clipped,
				meta[0] ? padding + 2 : 0, "", meta, style[0] ? ANSI_RESET : "");
}

int cli_jobs_render(struct cli_application *ctx, morph_buf_t *out, int columns, int max_jobs,
		    int styled)
{
	if (!ctx->jobs || columns < 20)
		return 0;
	int rows = 0, shown = 0, total = 0;
	/* Running work stays ahead of recent failures and successes. */
	for (int pass = 0; pass < 3; pass++) {
		struct cli_jobs_job *job;
		morph_array_foreach (job, &ctx->jobs->entries, struct cli_jobs_job) {
			if (!cli_jobs_job_visible(ctx, job))
				continue;
			int priority = cli_jobs_job_running(job->data)	? 0
				       : cli_jobs_job_failed(job->data) ? 1
									: 2;
			if (priority != pass)
				continue;
			total++;
			if (shown >= max_jobs)
				continue;
			shown++;
			const char *description =
				json_str_or(json_member(job->data, "description"), NULL, "");
			const char *marker = priority == 0   ? CLI_FEED_RUNNING
					     : priority == 1 ? "✗"
							     : "✓";
			const char *style = !styled	    ? ""
					    : priority == 0 ? ANSI_CYAN
					    : priority == 1 ? ANSI_RED
							    : ANSI_DIM;
			char prefix[32], meta[64];
			int64_t elapsed =
				(int64_t)json_number_value(json_member(job->data, "duration_ms"));
			if (priority == 0)
				elapsed += cli_jobs_now() - job->updated;
			if (priority == 1 &&
			    !strcmp(json_str_or(json_member(job->data, "status"), NULL, ""),
				    "exited"))
				snprintf(meta, sizeof(meta), "exit %d",
					 (int)json_number_value(
						 json_member(job->data, "exit_code")));
			else if (priority == 1)
				snprintf(meta, sizeof(meta), "%s",
					 json_str_or(json_member(job->data, "status"), NULL, ""));
			else
				snprintf(meta, sizeof(meta), "%llds", (long long)(elapsed / 1000));
			snprintf(prefix, sizeof(prefix), "  %s ", marker);
			if (description[0]) {
				(void)cli_jobs_job_row(out, prefix, description, "", columns,
						       style);
				rows++;
				(void)cli_jobs_job_row(
					out, "    └─ ",
					json_str_or(json_member(job->data, "command"), NULL, ""),
					meta, columns, styled ? ANSI_DIM : "");
			} else {
				(void)cli_jobs_job_row(
					out, prefix,
					json_str_or(json_member(job->data, "command"), NULL, ""),
					meta, columns, style);
			}
			rows++;
		}
	}
	if (total > shown) {
		char more[64];
		snprintf(more, sizeof(more), "+%d more · /jobs", total - shown);
		(void)cli_jobs_job_row(out, "  ", more, "", columns, styled ? ANSI_DIM : "");
		rows++;
	}
	return rows;
}

void cli_jobs_acknowledge(struct cli_application *ctx, const char *id)
{
	if (!ctx->jobs)
		return;
	struct cli_jobs_job *job;
	morph_array_foreach (job, &ctx->jobs->entries, struct cli_jobs_job) {
		if (!strcmp(json_str_or(json_member(job->data, "session_id"), NULL, ""), id))
			job->seen = 1;
	}
}

struct cli_jobs_view {
	struct cli_application *ctx;
	struct json *jobs;
	struct json *details;
	struct morph_tui_frame frame;
	struct morph_tui_widget_list_state selection;
	struct morph_tui_widget_text_layout lines;
	morph_buf_t document;
	int detail;
	int64_t refreshed;
	size_t scroll;
	const char *message;
};

static int cli_jobs_refresh(struct cli_jobs_view *view)
{
	struct json *jobs = NULL;
	int rc = runtime_process_list(view->ctx->runtime, &jobs);
	if (rc)
		return rc;
	struct json *selected = json_at(view->jobs, (int)view->selection.index);
	char *selected_id =
		selected ? strdup(json_str_or(json_member(selected, "session_id"), NULL, ""))
			 : NULL;
	if (selected && !selected_id) {
		json_free(jobs);
		return -ENOMEM;
	}
	json_free(view->jobs);
	view->jobs = jobs;
	if (selected_id) {
		struct json *entry;
		size_t index = 0;
		json_foreach (entry, jobs) {
			if (!strcmp(json_str_or(json_member(entry, "session_id"), NULL, ""),
				    selected_id)) {
				view->selection.index = index;
				break;
			}
			index++;
		}
		free(selected_id);
	}
	view->refreshed = cli_jobs_now();
	if (view->detail) {
		struct json *job = json_at(jobs, (int)view->selection.index);
		struct json *details = NULL;
		if (job &&
		    !runtime_process_read(view->ctx->runtime,
					  json_str_or(json_member(job, "session_id"), NULL, ""),
					  &details)) {
			json_free(view->details);
			view->details = details;
			if (!cli_jobs_job_running(details))
				cli_jobs_acknowledge(
					view->ctx,
					json_str_or(json_member(details, "session_id"), NULL, ""));
		}
	}
	return 0;
}

static int cli_jobs_document(struct cli_jobs_view *view)
{
	morph_buf_reset(&view->document);
	(void)morph_buf_printf(&view->document, "%s\n%s\nDirectory: %s\nStatus: %s",
			       json_str_or(json_member(view->details, "description"), NULL, ""),
			       json_str_or(json_member(view->details, "command"), NULL, ""),
			       json_str_or(json_member(view->details, "workdir"), NULL, ""),
			       json_str_or(json_member(view->details, "status"), NULL, ""));
	const struct json *exit_code = json_member(view->details, "exit_code");
	if (json_is_number_value(exit_code))
		(void)morph_buf_printf(&view->document, " · exit %d", json_int_value(exit_code));
	(void)morph_buf_printf(&view->document, "\n\n%s%s",
			       json_str_or(json_member(view->details, "stdout"), NULL, ""),
			       json_str_or(json_member(view->details, "stderr"), NULL, ""));
	if (json_is_true(json_member(view->details, "stdout_truncated")) ||
	    json_is_true(json_member(view->details, "stderr_truncated")))
		(void)morph_buf_puts(&view->document,
				     "\n[Output truncated; retained window shown]");
	return view->document.failed;
}

static int cli_jobs_draw(void *data, const struct morph_tui_screen *screen)
{
	struct cli_jobs_view *view = data;
	int columns = screen->area.width, rows = screen->area.height;
	morph_buf_t frame = {0}, line = {0};
	int rc = morph_buf_init(&frame, BUFSIZ);
	if (!rc)
		rc = morph_buf_init(&line, BUFSIZ);
	if (rc) {
		morph_buf_cleanup(&frame);
		return rc;
	}
	int page = rows > 5 ? rows - 5 : 1;
	morph_tui_widget_list_state_layout(&view->selection, (size_t)json_count(view->jobs),
					   (size_t)page);
	if (view->detail && view->details) {
		rc = cli_jobs_document(view);
		if (rc) {
			morph_buf_cleanup(&frame);
			morph_buf_cleanup(&line);
			return rc;
		}
		char *safe = utf8_terminal_sanitize_dup(morph_buf_cstr(&view->document),
							view->document.len,
							UTF8_TERMINAL_TEXT_MULTILINE, NULL);
		if (!safe)
			rc = -ENOMEM;
		if (!rc) {
			morph_buf_reset(&view->document);
			rc = morph_buf_puts(&view->document, safe);
		}
		free(safe);
		if (!rc)
			rc = morph_tui_widget_text_layout_wrap(
				&view->lines, morph_buf_cstr(&view->document),
				(size_t)(columns > 4 ? columns - 4 : 1));
		view->scroll = morph_tui_widget_scroll_update(view->scroll, view->lines.lines.nelts,
							      (size_t)page, 0);
	}
	for (int row = 0; row < rows && !rc; row++) {
		morph_buf_reset(&line);
		const char *style = "";
		if (!row) {
			(void)morph_buf_puts(&line, view->detail ? "  morph / jobs / output"
								 : "  morph / jobs");
			style = ANSI_BOLD;
		} else if (row >= 2 && row < page + 2) {
			if (view->detail) {
				struct morph_tui_widget_text_line *text = morph_array_get(
					&view->lines.lines, view->scroll + (size_t)(row - 2));
				if (text) {
					(void)morph_buf_puts(&line, "  ");
					(void)morph_buf_append(&line,
							       view->document.data + text->offset,
							       text->length);
				}
			} else {
				size_t index = view->selection.first + (size_t)(row - 2);
				struct json *job = json_at(view->jobs, (int)index);
				if (job) {
					(void)morph_buf_printf(
						&line, "  %.8s  %-11s  %s",
						cli_jobs_job_short_id(job),
						json_str_or(json_member(job, "status"), NULL, ""),
						json_str_or(json_member(job, "command"), NULL, ""));
					if (index == view->selection.index)
						style = ANSI_CYAN "\033[7m";
				} else if (!json_count(view->jobs) && row == 2)
					(void)morph_buf_puts(
						&line, "  No background jobs in this session.");
			}
		} else if (row == rows - 2 && view->message)
			(void)morph_buf_printf(&line, "  %s", view->message);
		else if (row == rows - 1)
			(void)morph_buf_puts(
				&line, "  Up/Down · Enter output · k stop · r refresh · Esc back");
		char compact[BUFSIZ], clipped[BUFSIZ];
		cli_list_compact_text(compact, sizeof(compact), morph_buf_cstr(&line));
		utf8_copy_ellipsized_display_width(clipped, sizeof(clipped), compact,
						   (size_t)(columns > 1 ? columns - 1 : 0), 0);
		rc = morph_buf_printf(&frame, "%s%s%s\n",
				      cli_output_color_enabled(view->ctx) ? style : "", clipped,
				      cli_output_color_enabled(view->ctx) ? ANSI_RESET : "");
	}
	if (!rc)
		rc = morph_tui_frame_present(&view->frame, stdout, columns, rows,
					     morph_buf_cstr(&frame));
	morph_buf_cleanup(&frame);
	morph_buf_cleanup(&line);
	return rc;
}

static int cli_jobs_update(void *data, const struct morph_tui_event *event)
{
	struct cli_jobs_view *view = data;
	if (event->type == MORPH_TUI_EVENT_TICK) {
		/* Yield the terminal to permission/interaction dialogs on the owner. */
		if (cli_event_queue_owner_call_pending(view->ctx))
			return MORPH_TUI_SCREEN_CLOSE;
		if (cli_jobs_now() - view->refreshed < 1000)
			return MORPH_TUI_SCREEN_CONTINUE;
		int rc = cli_jobs_refresh(view);
		return rc ? rc : MORPH_TUI_SCREEN_REDRAW;
	}
	if (event->type != MORPH_TUI_EVENT_KEY || event->pasting)
		return MORPH_TUI_SCREEN_CONTINUE;
	int key = event->key;
	if (key == 27 || key == 'q' || key == 3 || key == 4) {
		if (view->detail && key != 3 && key != 4)
			view->detail = 0;
		else
			return MORPH_TUI_SCREEN_CLOSE;
	} else if (key == 'k') {
		struct json *job = json_at(view->jobs, (int)view->selection.index);
		if (job) {
			int rc = runtime_process_stop(
				view->ctx->runtime,
				json_str_or(json_member(job, "session_id"), NULL, ""));
			view->message = rc ? morph_strerror(rc) : "Stop requested";
		}
	} else if (key == '\r' || key == '\n') {
		view->detail = 1;
		view->scroll = 0;
	} else if (view->detail) {
		if (key == MORPH_TUI_KEY_UP && view->scroll)
			view->scroll--;
		if (key == MORPH_TUI_KEY_DOWN)
			view->scroll++;
		if (key == MORPH_TUI_KEY_PAGE_UP)
			view->scroll = view->scroll > 10 ? view->scroll - 10 : 0;
		if (key == MORPH_TUI_KEY_PAGE_DOWN)
			view->scroll += 10;
		if (key == MORPH_TUI_KEY_HOME)
			view->scroll = 0;
		if (key == MORPH_TUI_KEY_END)
			view->scroll = view->lines.lines.nelts;
	} else {
		(void)morph_tui_widget_list_state_update(&view->selection, key);
	}
	int rc = cli_jobs_refresh(view);
	return rc ? rc : MORPH_TUI_SCREEN_REDRAW;
}

static void cli_jobs_view_cleanup(struct cli_jobs_view *view)
{
	json_free(view->jobs);
	json_free(view->details);
	morph_buf_cleanup(&view->document);
	morph_tui_frame_cleanup(&view->frame);
	morph_tui_widget_text_layout_cleanup(&view->lines);
}

struct cli_jobs_controller {
	struct morph_tui_view_controller base;
	struct cli_jobs_view view;
	struct morph_tui_view *title, *list, *document, *message;
	char row[BUFSIZ];
	int presented;
};

static const char *cli_jobs_controller_row(void *data, size_t index)
{
	struct cli_jobs_controller *page = data;
	struct json *job = json_at(page->view.jobs, (int)index);

	snprintf(page->row, sizeof(page->row), "%.8s  %-11s  %s", cli_jobs_job_short_id(job),
		 json_str_or(json_member(job, "status"), NULL, ""),
		 json_str_or(json_member(job, "command"), NULL, ""));
	return page->row;
}

static int cli_jobs_controller_sync(struct cli_jobs_controller *page, int focus)
{
	struct cli_jobs_view *view = &page->view;
	size_t count = (size_t)json_count(view->jobs);
	int rc = morph_tui_widget_list_set_source(page->list, count, cli_jobs_controller_row, page);

	morph_tui_widget_list_state_layout(&view->selection, count, view->selection.page);
	if (!rc && count)
		rc = morph_tui_widget_list_select(page->list, view->selection.index);
	if (!rc)
		rc = morph_tui_view_set_hidden(page->list, view->detail);
	if (!rc)
		rc = morph_tui_view_set_hidden(page->document, !view->detail);
	if (!rc)
		rc = morph_tui_widget_label_set_text(
			page->title, view->detail ? "morph / jobs / output" : "morph / jobs");
	if (!rc)
		rc = morph_tui_widget_label_set_text(
			page->message,
			view->message ? view->message
				      : (!count ? "No background jobs in this session." : ""));
	if (!rc && view->detail && view->details) {
		rc = cli_jobs_document(view);
		if (!rc)
			rc = morph_tui_widget_text_view_set_text(page->document,
								 morph_buf_cstr(&view->document));
	}
	if (!rc && focus)
		rc = morph_tui_view_focus(view->detail ? page->document : page->list);
	return rc;
}

static void cli_jobs_controller_build(struct morph_tui_view_controller *controller,
				      struct morph_tui_builder *builder)
{
	struct cli_jobs_controller *page = (struct cli_jobs_controller *)controller;
	struct cli_jobs_view *source = controller->data;

	/* Move the model into the controller. The caller's cleanup remains valid
	 * when allocation fails before build or construction fails after the move. */
	page->view = *source;
	memset(source, 0, sizeof(*source));
	controller->data = NULL;
	const struct morph_tui_view_options inset = {.padding = {1, 1, 1, 1}};
	const struct morph_tui_view_options fill = {.grow = 1};
	struct morph_tui_view *root = morph_tui_widget_stack_create(
		builder, NULL, MORPH_TUI_WIDGET_STACK_VERTICAL, 0, &inset);

	page->title = morph_tui_widget_label_create(builder, root, "morph / jobs", NULL);
	morph_tui_widget_label_create(builder, root, "", NULL);
	page->list = morph_tui_widget_list_create(builder, root, NULL, 0, 0, &fill);
	page->document = morph_tui_widget_text_view_create(builder, root, "", &fill);
	page->message = morph_tui_widget_label_create(builder, root, "", NULL);
	morph_tui_widget_label_create(
		builder, root, "Up/Down · Enter output · k stop · r refresh · Esc back", NULL);
	if (!morph_tui_builder_error(builder)) {
		morph_tui_widget_list_set_navigation(page->list, 0);
		morph_tui_builder_fail(builder, cli_jobs_controller_sync(page, 1));
	}
}

static int cli_jobs_controller_action(struct morph_tui_view_controller *controller,
				      const struct morph_tui_widget_action *action)
{
	struct cli_jobs_controller *page = (struct cli_jobs_controller *)controller;

	page->view.selection.index = action->index;
	if (action->type != MORPH_TUI_WIDGET_ACTION_ACTIVATE)
		return 0;
	page->view.detail = 1;
	int rc = cli_jobs_refresh(&page->view);

	return rc ? rc : cli_jobs_controller_sync(page, 1);
}

static int cli_jobs_controller_event(struct morph_tui_view_controller *controller,
				     const struct morph_tui_event *event)
{
	struct cli_jobs_controller *page = (struct cli_jobs_controller *)controller;
	struct cli_jobs_view *view = &page->view;
	int focus = 0;

	if (event->type == MORPH_TUI_EVENT_TICK) {
		if (cli_jobs_now() - view->refreshed < 1000)
			return 0;
	} else if (event->type == MORPH_TUI_EVENT_KEY || event->type == MORPH_TUI_EVENT_TEXT) {
		if ((event->key == 27 || event->key == 'q') && view->detail) {
			view->detail = 0;
			focus = 1;
		} else if (event->key == 'k') {
			struct json *job = json_at(view->jobs, (int)view->selection.index);

			if (job) {
				int rc = runtime_process_stop(
					view->ctx->runtime,
					json_str_or(json_member(job, "session_id"), NULL, ""));

				view->message = rc ? morph_strerror(rc) : "Stop requested";
			}
		} else if (event->key != 'r')
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
}

static int cli_jobs_controller_appear(struct morph_tui_view_controller *controller)
{
	((struct cli_jobs_controller *)controller)->presented = 1;
	return 0;
}

static void cli_jobs_controller_destroy(struct morph_tui_view_controller *controller, int result)
{
	struct cli_jobs_controller *page = (struct cli_jobs_controller *)controller;

	if (page->presented)
		cli_application_panel_complete(page->view.ctx, page, cli_jobs_controller_cleanup,
					       result);
	else
		cli_jobs_controller_cleanup(page, result);
}

int cli_jobs_view(struct cli_application *ctx, const char *id)
{
	struct cli_jobs_view view = {.ctx = ctx};
	struct morph_tui_screen screen = {0};
	const struct morph_tui_screen_ops ops = {.draw = cli_jobs_draw, .update = cli_jobs_update};
	int rc = morph_buf_init(&view.document, BUFSIZ);
	if (!rc)
		rc = cli_jobs_refresh(&view);
	if (!rc && id) {
		struct json *details = NULL;
		rc = runtime_process_read(ctx->runtime, id, &details);
		if (!rc) {
			struct json *job;
			size_t index = 0;
			json_foreach (job, view.jobs) {
				if (!strcmp(json_str_or(json_member(job, "session_id"), NULL, ""),
					    json_str_or(json_member(details, "session_id"), NULL,
							""))) {
					view.selection.index = index;
					view.detail = 1;
					break;
				}
				index++;
			}
			if (!view.detail)
				rc = -ENOENT;
			else
				rc = cli_jobs_refresh(&view);
		}
		json_free(details);
	}
	if (!rc && cli_application_tui(ctx)) {
		struct cli_jobs_controller page;
		static const struct morph_tui_view_controller_ops controller_ops = {
			.build = cli_jobs_controller_build,
			.action = cli_jobs_controller_action,
			.event = cli_jobs_controller_event,
			.destroy = cli_jobs_controller_destroy,
			.appear = cli_jobs_controller_appear,
		};
		const struct morph_tui_view_controller_spec spec = {
			.ops = &controller_ops,
			.data = &view,
			.size = sizeof(page),
			.quit_key = 'q',
			.tick_ms = 1000,
		};

		rc = morph_tui_view_controller_present(cli_application_tui(ctx), &spec);
		if (!rc)
			return 0;
	}

	screen.interrupted = &ctx->interrupted;
	if (!rc)
		rc = morph_tui_screen_open(&screen, stdin, stdout,
					   MORPH_TUI_TERMINAL_SESSION_ALTERNATE |
						   MORPH_TUI_TERMINAL_SESSION_PASTE);
	if (!rc)
		rc = morph_tui_screen_run(&screen, &ops, &view);
	int leave_rc = morph_tui_screen_close(&screen);
	if (rc == -EINTR)
		rc = 0;
	if (!rc)
		rc = leave_rc;
	morph_signal_flag_set(&ctx->interrupted, 0);
	json_free(view.jobs);
	json_free(view.details);
	morph_buf_cleanup(&view.document);
	morph_tui_frame_cleanup(&view.frame);
	morph_tui_widget_text_layout_cleanup(&view.lines);
	return rc;
}
