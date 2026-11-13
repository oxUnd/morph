#include "sapi/cli/pages/jobs.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "cJSON.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/application.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/feed_block.h"
#include "util/cleanup.h"
#include "sapi/cli/widgets/list.h"
#include <morph/tui/components.h>
#include <morph/tui/view_controller.h>
#include <morph/tui/widgets/label.h>
#include <morph/tui/widgets/stack.h>

struct cli_jobs_job {
	cJSON *data;
	int64_t updated;
	int waiting;
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

static const char *cli_jobs_job_string(const cJSON *data, const char *key)
{
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, key));
	return text ? text : "";
}

static const char *cli_jobs_job_short_id(const cJSON *data)
{
	const char *id = cli_jobs_job_string(data, "session_id");
	return !strncmp(id, "proc_", 5) ? id + 5 : id;
}

static int cli_jobs_job_running(const cJSON *data)
{
	const char *state = cli_jobs_job_string(data, "status");
	return !strcmp(state, "running") || !strcmp(state, "starting");
}

static int cli_jobs_job_cancelled(const cJSON *data)
{
	const char *status = cli_jobs_job_string(data, "status");
	return !strcmp(status, "killed") || !strcmp(status, "cancelled") ||
	       !strcmp(status, "interrupted");
}

static int cli_jobs_job_failed(const cJSON *data)
{
	return !cli_jobs_job_running(data) &&
	       (strcmp(cli_jobs_job_string(data, "status"), "exited") ||
		cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(data, "exit_code")) != 0);
}

static int cli_jobs_job_visible(const struct cli_application *ctx, const struct cli_jobs_job *job)
{
	int64_t owner = 0;
	(void)runtime_session_current_id(ctx->runtime, &owner);
	if (cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(job->data, "owner_session_id")) !=
	    (double)owner)
		return 0;
	return cli_jobs_job_running(job->data);
}

static int cli_jobs_remember_job(struct cli_application *ctx, const cJSON *data)
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
	cJSON *copy = cJSON_Duplicate(data, 1);
	if (!copy)
		return -ENOMEM;
	struct cli_jobs_job *job;
	morph_array_foreach (job, &ctx->jobs->entries, struct cli_jobs_job) {
		if (!strcmp(cli_jobs_job_string(job->data, "session_id"),
			    cli_jobs_job_string(data, "session_id"))) {
			cJSON_Delete(job->data);
			job->data = copy;
			job->updated = cli_jobs_now();
			return 0;
		}
	}
	job = morph_array_push_borrow(&ctx->jobs->entries);
	if (!job) {
		cJSON_Delete(copy);
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
		cJSON_Delete(job->data);
	morph_array_cleanup(&ctx->jobs->entries);
	free(ctx->jobs);
	ctx->jobs = NULL;
}

int cli_jobs_event(struct cli_application *ctx, const struct morph_event *event)
{
	if (event->name && (!strcmp(event->name, "background.wait.begin") ||
			    !strcmp(event->name, "background.wait.end"))) {
		if (ctx->jobs) {
			struct cli_jobs_job *job;
			morph_array_foreach (job, &ctx->jobs->entries, struct cli_jobs_job) {
				if (!strcmp(cli_jobs_job_string(job->data, "session_id"),
					    cli_jobs_job_string(event->data, "session_id"))) {
					if (!strcmp(event->name, "background.wait.begin"))
						job->waiting++;
					else if (job->waiting > 0)
						job->waiting--;
				}
			}
		}
		cli_feed_queue_changed(ctx);
		return 1;
	}
	if (!event->name ||
	    (strcmp(event->name, "process.started") && strcmp(event->name, "process.completed")))
		return 0;
	int rc = cli_jobs_remember_job(ctx, event->data);
	if (rc)
		return rc;
	if (!strcmp(event->name, "process.completed")) {
		int64_t owner = 0;
		(void)runtime_session_current_id(ctx->runtime, &owner);
		if (cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
			    event->data, "owner_session_id")) == (double)owner) {
			const char *description = cli_jobs_job_string(event->data, "description");
			char safe[BUFSIZ];
			(void)cli_list_compact_text(
				safe, sizeof(safe),
				description[0] ? description
					       : cli_jobs_job_string(event->data, "command"));
			cli_feed_history_begin(ctx);
			if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE) {
				morph_buf_t title MORPH_CLEANUP(morph_buf_cleanup) = {0};
				const char *status = cli_jobs_job_string(event->data, "status");
				int timeout = strstr(status, "timeout") || strstr(status, "timed_out");
				(void)morph_buf_printf(&title, "%s · %s", safe, timeout ? "TIMEOUT" : status);
				const cJSON *exit_code = cJSON_GetObjectItemCaseSensitive(event->data, "exit_code");
				if (cJSON_IsNumber(exit_code))
					(void)morph_buf_printf(&title, " %d", exit_code->valueint);
				(void)morph_buf_printf(&title, " · /jobs %.8s", cli_jobs_job_short_id(event->data));
				(void)cli_feed_notice_write(ctx,
					cli_jobs_job_cancelled(event->data) ? CLI_NOTICE_CANCELLED
					: cli_jobs_job_failed(event->data) ? CLI_NOTICE_FAILED : CLI_NOTICE_DONE,
					morph_buf_cstr_borrow(&title), NULL, NULL);
			} else {
			printf(ctx, "%s%s" ANSI_RESET " %s · %s",
			       cli_jobs_job_cancelled(event->data) ? ANSI_DIM
			       : cli_jobs_job_failed(event->data)
				 ? CLI_FEED_FAILED_STYLE : ANSI_GREEN,
			       cli_jobs_job_cancelled(event->data) ? CLI_FEED_CANCELLED
			       : cli_jobs_job_failed(event->data) ? CLI_FEED_FAILED : CLI_FEED_DONE,
			       safe, cli_jobs_job_string(event->data, "status"));
			const cJSON *exit_code =
				cJSON_GetObjectItemCaseSensitive(event->data, "exit_code");
			if (cJSON_IsNumber(exit_code))
				printf(ctx, " %d", exit_code->valueint);
			printf(ctx, " · /jobs %.8s\n", cli_jobs_job_short_id(event->data));
			}
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
			    const char *meta, int columns, const char *style,
			    const char *prefix_style)
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
	return morph_buf_printf(out, "%s%s%s%s%s%*s%s%s",
				prefix_style ? prefix_style : style, prefix,
				prefix_style ? ANSI_RESET : "", prefix_style ? style : "", clipped,
				meta[0] ? padding + 2 : 0, "", meta, style[0] ? ANSI_RESET : "");
}

int cli_jobs_waiting(const struct cli_application *ctx)
{
	if (!ctx || !ctx->jobs)
		return 0;
	struct cli_jobs_job *job;
	morph_array_foreach (job, &ctx->jobs->entries, struct cli_jobs_job) {
		if (job->waiting && cli_jobs_job_visible(ctx, job))
			return 1;
	}
	return 0;
}

int cli_jobs_render(struct cli_application *ctx, morph_buf_t *out, int columns, int styled)
{
	if (!ctx->jobs || columns < 20)
		return 0;
	int total = 0;
	const struct cli_jobs_job *waiting = NULL;
	struct cli_jobs_job *job;
	morph_array_foreach (job, &ctx->jobs->entries, struct cli_jobs_job) {
		if (!cli_jobs_job_visible(ctx, job))
			continue;
		total++;
		if (job->waiting && !waiting)
			waiting = job;
	}
	if (!total)
		return 0;
	morph_buf_t summary MORPH_CLEANUP(morph_buf_cleanup) = {0};
	if (morph_buf_init(&summary, 128))
		return 0;
	if (waiting) {
		int64_t elapsed = (int64_t)cJSON_GetNumberValue(
			cJSON_GetObjectItemCaseSensitive(waiting->data, "duration_ms"));
		int64_t seconds = (elapsed + cli_jobs_now() - waiting->updated) / 1000;
		(void)morph_buf_puts(&summary, columns < 60 ? "Waiting for job · "
							 : "Waiting for background job (");
		if (seconds >= 60)
			(void)morph_buf_printf(&summary, "%lldm ", (long long)(seconds / 60));
		(void)morph_buf_printf(&summary, "%llds", (long long)(seconds % 60));
		(void)morph_buf_puts(&summary, columns < 60 ? " · Esc" :
			columns < 90 ? " · Esc)" : " · Esc to interrupt wait)");
		if (columns >= 100)
			(void)morph_buf_printf(&summary, " · %d running", total);
	} else {
		(void)morph_buf_printf(&summary, "%d background job%s%s", total,
				       total == 1 ? "" : "s", columns < 60 ? "" : " running");
	}
	(void)morph_buf_puts(&summary, columns < 100 ? " · /jobs" : " · /jobs to view");
	(void)cli_jobs_job_row(out, CLI_FEED_RUNNING " ",
			       morph_buf_cstr_borrow(&summary), "", columns,
			       styled ? ANSI_CYAN : "",
			       styled ? cli_style_running_color(cli_jobs_now()) : NULL);
	if (!waiting)
		return 1;
	(void)cli_jobs_job_row(out, "  └ ", cli_jobs_job_string(waiting->data, "command"),
			       "", columns, styled ? ANSI_DIM : "", NULL);
	return 2;
}

struct cli_jobs_view {
	struct cli_application *ctx;
	cJSON *jobs;
	cJSON *details;
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
	cJSON *jobs = NULL;
	int rc = runtime_process_list(view->ctx->runtime, &jobs);
	if (rc)
		return rc;
	cJSON *selected = cJSON_GetArrayItem(view->jobs, (int)view->selection.index);
	char *selected_id = selected ? strdup(cli_jobs_job_string(selected, "session_id")) : NULL;
	if (selected && !selected_id) {
		cJSON_Delete(jobs);
		return -ENOMEM;
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
	morph_buf_reset(&view->document);
	(void)morph_buf_printf(&view->document, "%s\n%s\nDirectory: %s\nStatus: %s",
			       cli_jobs_job_string(view->details, "description"),
			       cli_jobs_job_string(view->details, "command"),
			       cli_jobs_job_string(view->details, "workdir"),
			       cli_jobs_job_string(view->details, "status"));
	const cJSON *exit_code = cJSON_GetObjectItemCaseSensitive(view->details, "exit_code");
	if (cJSON_IsNumber(exit_code))
		(void)morph_buf_printf(&view->document, " · exit %d", exit_code->valueint);
	(void)morph_buf_printf(&view->document, "\n\n%s%s",
			       cli_jobs_job_string(view->details, "stdout"),
			       cli_jobs_job_string(view->details, "stderr"));
	if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(view->details, "stdout_truncated")) ||
	    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(view->details, "stderr_truncated")))
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
	morph_tui_widget_list_state_layout(&view->selection, (size_t)cJSON_GetArraySize(view->jobs),
					   (size_t)page);
	if (view->detail && view->details) {
		rc = cli_jobs_document(view);
		if (rc) {
			morph_buf_cleanup(&frame);
			morph_buf_cleanup(&line);
			return rc;
		}
		char *safe = utf8_terminal_sanitize_dup(morph_buf_cstr_borrow(&view->document),
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
			rc = morph_tui_widget_text_layout_wrap_borrow(
				&view->lines, morph_buf_cstr_borrow(&view->document),
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
				struct morph_tui_widget_text_line *text = morph_array_borrow(
					&view->lines.lines, view->scroll + (size_t)(row - 2));
				if (text) {
					(void)morph_buf_puts(&line, "  ");
					(void)morph_buf_append(&line,
							       view->document.data + text->offset,
							       text->length);
				}
			} else {
				size_t index = view->selection.first + (size_t)(row - 2);
				cJSON *job = cJSON_GetArrayItem(view->jobs, (int)index);
				if (job) {
					(void)morph_buf_printf(&line, "  %.8s  %-11s  %s",
							       cli_jobs_job_short_id(job),
							       cli_jobs_job_string(job, "status"),
							       cli_jobs_job_string(job, "command"));
					if (index == view->selection.index)
						style = ANSI_CYAN "\033[7m";
				} else if (!cJSON_GetArraySize(view->jobs) && row == 2)
					(void)morph_buf_puts(
						&line, "  No background jobs in this session.");
			}
		} else if (row == rows - 2 && view->message)
			(void)morph_buf_printf(&line, "  %s", view->message);
		else if (row == rows - 1)
			(void)morph_buf_puts(
				&line, "  Up/Down · Enter output · k stop · r refresh · Esc back");
		char compact[BUFSIZ], clipped[BUFSIZ];
		cli_list_compact_text(compact, sizeof(compact), morph_buf_cstr_borrow(&line));
		utf8_copy_ellipsized_display_width(clipped, sizeof(clipped), compact,
						   (size_t)(columns > 1 ? columns - 1 : 0), 0);
		rc = morph_buf_printf(&frame, "%s%s%s\n",
				      cli_output_color_enabled(view->ctx) ? style : "", clipped,
				      cli_output_color_enabled(view->ctx) ? ANSI_RESET : "");
	}
	if (!rc)
		rc = morph_tui_frame_present(&view->frame, stdout, columns, rows,
					     morph_buf_cstr_borrow(&frame));
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
		cJSON *job = cJSON_GetArrayItem(view->jobs, (int)view->selection.index);
		if (job) {
			int rc = runtime_process_stop(view->ctx->runtime,
						      cli_jobs_job_string(job, "session_id"));
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
	cJSON_Delete(view->jobs);
	cJSON_Delete(view->details);
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
	cJSON *job = cJSON_GetArrayItem(page->view.jobs, (int)index);

	snprintf(page->row, sizeof(page->row), "%.8s  %-11s  %s", cli_jobs_job_short_id(job),
		 cli_jobs_job_string(job, "status"), cli_jobs_job_string(job, "command"));
	return page->row;
}

static int cli_jobs_controller_sync(struct cli_jobs_controller *page, int focus)
{
	struct cli_jobs_view *view = &page->view;
	size_t count = (size_t)cJSON_GetArraySize(view->jobs);
	int rc = morph_tui_widget_list_set_source_borrow(page->list, count, cli_jobs_controller_row,
							 page);

	morph_tui_widget_list_state_layout(&view->selection, count, view->selection.page);
	if (!rc && count)
		rc = morph_tui_widget_list_select(page->list, view->selection.index);
	if (!rc)
		rc = morph_tui_view_set_hidden(page->list, view->detail);
	if (!rc)
		rc = morph_tui_view_set_hidden(page->document, !view->detail);
	if (!rc)
		rc = morph_tui_widget_label_set_text_copy(
			page->title, view->detail ? "morph / jobs / output" : "morph / jobs");
	if (!rc)
		rc = morph_tui_widget_label_set_text_copy(
			page->message,
			view->message ? view->message
				      : (!count ? "No background jobs in this session." : ""));
	if (!rc && view->detail && view->details) {
		rc = cli_jobs_document(view);
		if (!rc)
			rc = morph_tui_widget_text_view_set_text_copy(
				page->document, morph_buf_cstr_borrow(&view->document));
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
	struct morph_tui_view *root = morph_tui_widget_stack_add_borrow(
		builder, NULL, MORPH_TUI_WIDGET_STACK_VERTICAL, 0, &inset);

	page->title = morph_tui_widget_label_add_borrow(builder, root, "morph / jobs", NULL);
	morph_tui_widget_label_add_borrow(builder, root, "", NULL);
	page->list = morph_tui_widget_list_add_borrow(builder, root, NULL, 0, 0, &fill);
	page->document = morph_tui_widget_text_view_add_borrow(builder, root, "", &fill);
	page->message = morph_tui_widget_label_add_borrow(builder, root, "", NULL);
	morph_tui_widget_label_add_borrow(
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
			cJSON *job = cJSON_GetArrayItem(view->jobs, (int)view->selection.index);

			if (job) {
				int rc = runtime_process_stop(
					view->ctx->runtime, cli_jobs_job_string(job, "session_id"));

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
		cJSON *details = NULL;
		rc = runtime_process_read(ctx->runtime, id, &details);
		if (!rc) {
			cJSON *job;
			size_t index = 0;
			cJSON_ArrayForEach (job, view.jobs) {
				if (!strcmp(cli_jobs_job_string(job, "session_id"),
					    cli_jobs_job_string(details, "session_id"))) {
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
		cJSON_Delete(details);
	}
	if (!rc && cli_application_tui_borrow(ctx)) {
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

		rc = morph_tui_view_controller_present_borrow(cli_application_tui_borrow(ctx),
							      &spec);
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
	cJSON_Delete(view.jobs);
	cJSON_Delete(view.details);
	morph_buf_cleanup(&view.document);
	morph_tui_frame_cleanup(&view.frame);
	morph_tui_widget_text_layout_cleanup(&view.lines);
	return rc;
}
