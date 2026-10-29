#include "sapi/cli/internal.h"
#include "sapi/cli/jobs_ui.h"
#include "sapi/cli/terminal.h"
#include "sapi/cli/list_ui.h"
#include <morph/tui/components.h>

#define CLI_JOB_SUCCESS_MS 5000

struct cli_job {
	cJSON *data;
	int64_t updated;
	int seen;
};

struct cli_jobs {
	morph_array_t entries;
};

static int64_t jobs_now(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return 0;
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static const char *job_string(const cJSON *data, const char *key)
{
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, key));
	return text ? text : "";
}

static const char *job_short_id(const cJSON *data)
{
	const char *id = job_string(data, "session_id");
	return !strncmp(id, "proc_", 5) ? id + 5 : id;
}

static int job_running(const cJSON *data)
{
	const char *state = job_string(data, "status");
	return !strcmp(state, "running") || !strcmp(state, "starting");
}

static int job_failed(const cJSON *data)
{
	return !job_running(data) &&
	       (strcmp(job_string(data, "status"), "exited") ||
		cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(data, "exit_code")) != 0);
}

static int job_visible(const struct cli_context *ctx, const struct cli_job *job)
{
	int64_t owner = 0;
	(void)runtime_session_current_id(ctx->runtime, &owner);
	if (cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(job->data, "owner_session_id")) !=
	    (double)owner)
		return 0;
	return job_running(job->data) || (!job->seen && job_failed(job->data)) ||
	       (!job->seen && jobs_now() - job->updated < CLI_JOB_SUCCESS_MS);
}

static int remember_job(struct cli_context *ctx, const cJSON *data)
{
	if (!ctx->jobs) {
		struct cli_job entry;
		ctx->jobs = calloc(1, sizeof(*ctx->jobs));
		if (!ctx->jobs)
			MORPH_RETURN(-ENOMEM);
		int rc = morph_array_init(&ctx->jobs->entries, 8, sizeof(entry));
		if (rc) {
			free(ctx->jobs);
			ctx->jobs = NULL;
			return rc;
		}
	}
	cJSON *copy = cJSON_Duplicate(data, 1);
	if (!copy)
		MORPH_RETURN(-ENOMEM);
	struct cli_job *job;
	morph_array_foreach(job, &ctx->jobs->entries, struct cli_job)
	{
		if (!strcmp(job_string(job->data, "session_id"), job_string(data, "session_id"))) {
			cJSON_Delete(job->data);
			job->data = copy;
			job->updated = jobs_now();
			return 0;
		}
	}
	job = morph_array_push(&ctx->jobs->entries);
	if (!job) {
		cJSON_Delete(copy);
		MORPH_RETURN(-ENOMEM);
	}
	*job = (struct cli_job){.data = copy, .updated = jobs_now()};
	return 0;
}

void cli_jobs_cleanup(struct cli_context *ctx)
{
	if (!ctx->jobs)
		return;
	struct cli_job *job;
	morph_array_foreach(job, &ctx->jobs->entries, struct cli_job) cJSON_Delete(job->data);
	morph_array_cleanup(&ctx->jobs->entries);
	free(ctx->jobs);
	ctx->jobs = NULL;
}

int cli_jobs_event(struct cli_context *ctx, const struct morph_event *event)
{
	if (!event->name ||
	    (strcmp(event->name, "process.started") && strcmp(event->name, "process.completed")))
		return 0;
	int rc = remember_job(ctx, event->data);
	if (rc)
		return rc;
	if (!strcmp(event->name, "process.completed")) {
		int64_t owner = 0;
		(void)runtime_session_current_id(ctx->runtime, &owner);
		if (cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
			    event->data, "owner_session_id")) == (double)owner) {
			const char *description = job_string(event->data, "description");
			char safe[BUFSIZ];
			(void)cli_list_compact_text(
				safe, sizeof(safe),
				description[0] ? description : job_string(event->data, "command"));
			cli_terminal_history_begin(ctx);
			printf(ctx, "  %s %s · %s", job_failed(event->data) ? "✗" : "✓", safe,
			       job_string(event->data, "status"));
			const cJSON *exit_code =
				cJSON_GetObjectItemCaseSensitive(event->data, "exit_code");
			if (cJSON_IsNumber(exit_code))
				printf(ctx, " %d", exit_code->valueint);
			printf(ctx, " · /jobs %.8s\n", job_short_id(event->data));
			cli_terminal_history_end(ctx);
		}
	}
	cli_terminal_queue_changed(ctx);
	return 1;
}

int cli_jobs_visible(const struct cli_context *ctx)
{
	if (!ctx->jobs)
		return 0;
	struct cli_job *job;
	morph_array_foreach(job, &ctx->jobs->entries, struct cli_job)
	{
		if (job_visible(ctx, job))
			return 1;
	}
	return 0;
}

static int job_row(morph_buf_t *out, const char *prefix, const char *text, const char *meta,
		   int columns, const char *style)
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

int cli_jobs_render(struct cli_context *ctx, morph_buf_t *out, int columns, int max_jobs,
		    int styled)
{
	if (!ctx->jobs || columns < 20)
		return 0;
	int rows = 0, shown = 0, total = 0;
	/* Running work stays ahead of retained failures and recent successes. */
	for (int pass = 0; pass < 3; pass++) {
		struct cli_job *job;
		morph_array_foreach(job, &ctx->jobs->entries, struct cli_job)
		{
			if (!job_visible(ctx, job))
				continue;
			int priority = job_running(job->data) ? 0 : job_failed(job->data) ? 1 : 2;
			if (priority != pass)
				continue;
			total++;
			if (shown >= max_jobs)
				continue;
			shown++;
			const char *description = job_string(job->data, "description");
			const char *marker = priority == 0   ? CLI_FEED_RUNNING
					     : priority == 1 ? "✗"
							     : "✓";
			const char *style = !styled	    ? ""
					    : priority == 0 ? ANSI_CYAN
					    : priority == 1 ? ANSI_RED
							    : ANSI_DIM;
			char prefix[32], meta[64];
			int64_t elapsed = (int64_t)cJSON_GetNumberValue(
				cJSON_GetObjectItemCaseSensitive(job->data, "duration_ms"));
			if (priority == 0)
				elapsed += jobs_now() - job->updated;
			if (priority == 1 && !strcmp(job_string(job->data, "status"), "exited"))
				snprintf(meta, sizeof(meta), "exit %d",
					 (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
						 job->data, "exit_code")));
			else if (priority == 1)
				snprintf(meta, sizeof(meta), "%s", job_string(job->data, "status"));
			else
				snprintf(meta, sizeof(meta), "%llds", (long long)(elapsed / 1000));
			snprintf(prefix, sizeof(prefix), "  %s ", marker);
			if (description[0]) {
				(void)job_row(out, prefix, description, "", columns, style);
				rows++;
				(void)job_row(out, "    └─ ", job_string(job->data, "command"),
					      meta, columns, styled ? ANSI_DIM : "");
			} else {
				(void)job_row(out, prefix, job_string(job->data, "command"), meta,
					      columns, style);
			}
			rows++;
		}
	}
	if (total > shown) {
		char more[64];
		snprintf(more, sizeof(more), "+%d more · /jobs", total - shown);
		(void)job_row(out, "  ", more, "", columns, styled ? ANSI_DIM : "");
		rows++;
	}
	return rows;
}

void cli_jobs_acknowledge(struct cli_context *ctx, const char *id)
{
	if (!ctx->jobs)
		return;
	struct cli_job *job;
	morph_array_foreach(job, &ctx->jobs->entries, struct cli_job)
	{
		if (!strcmp(job_string(job->data, "session_id"), id))
			job->seen = 1;
	}
}

struct jobs_view {
	struct cli_context *ctx;
	cJSON *jobs;
	cJSON *details;
	struct morph_tui_frame frame;
	struct morph_tui_list selection;
	struct morph_tui_text_view lines;
	morph_buf_t document;
	int detail;
	int64_t refreshed;
	size_t scroll;
	const char *message;
};

static int jobs_refresh(struct jobs_view *view)
{
	cJSON *jobs = NULL;
	int rc = runtime_process_list(view->ctx->runtime, &jobs);
	if (rc)
		return rc;
	cJSON *selected = cJSON_GetArrayItem(view->jobs, (int)view->selection.index);
	char *selected_id = selected ? strdup(job_string(selected, "session_id")) : NULL;
	if (selected && !selected_id) {
		cJSON_Delete(jobs);
		MORPH_RETURN(-ENOMEM);
	}
	cJSON_Delete(view->jobs);
	view->jobs = jobs;
	if (selected_id) {
		cJSON *entry;
		size_t index = 0;
		cJSON_ArrayForEach(entry, jobs)
		{
			if (!strcmp(job_string(entry, "session_id"), selected_id)) {
				view->selection.index = index;
				break;
			}
			index++;
		}
		free(selected_id);
	}
	view->refreshed = jobs_now();
	if (view->detail) {
		cJSON *job = cJSON_GetArrayItem(jobs, (int)view->selection.index);
		cJSON *details = NULL;
		if (job && !runtime_process_read(view->ctx->runtime, job_string(job, "session_id"),
						 &details)) {
			cJSON_Delete(view->details);
			view->details = details;
			if (!job_running(details))
				cli_jobs_acknowledge(view->ctx, job_string(details, "session_id"));
		}
	}
	return 0;
}

static int jobs_draw(void *data, const struct morph_tui_screen *screen)
{
	struct jobs_view *view = data;
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
	morph_tui_list_layout(&view->selection, (size_t)cJSON_GetArraySize(view->jobs),
			      (size_t)page);
	if (view->detail && view->details) {
		morph_buf_reset(&view->document);
		(void)morph_buf_printf(&view->document, "%s\n%s\nDirectory: %s\nStatus: %s",
				       job_string(view->details, "description"),
				       job_string(view->details, "command"),
				       job_string(view->details, "workdir"),
				       job_string(view->details, "status"));
		const cJSON *exit_code =
			cJSON_GetObjectItemCaseSensitive(view->details, "exit_code");
		if (cJSON_IsNumber(exit_code))
			(void)morph_buf_printf(&view->document, " · exit %d", exit_code->valueint);
		(void)morph_buf_printf(&view->document, "\n\n%s%s",
				       job_string(view->details, "stdout"),
				       job_string(view->details, "stderr"));
		if (cJSON_IsTrue(
			    cJSON_GetObjectItemCaseSensitive(view->details, "stdout_truncated")) ||
		    cJSON_IsTrue(
			    cJSON_GetObjectItemCaseSensitive(view->details, "stderr_truncated")))
			(void)morph_buf_puts(&view->document,
					     "\n[Output truncated; retained window shown]");
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
			rc = morph_tui_text_view_wrap(&view->lines, morph_buf_cstr(&view->document),
						      (size_t)(columns > 4 ? columns - 4 : 1));
		view->scroll = morph_tui_scroll_update(view->scroll, view->lines.lines.nelts,
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
				struct morph_tui_text_line *text = morph_array_get(
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
							       job_short_id(job),
							       job_string(job, "status"),
							       job_string(job, "command"));
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
		cli_list_compact_text(compact, sizeof(compact), morph_buf_cstr(&line));
		utf8_copy_ellipsized_display_width(clipped, sizeof(clipped), compact,
						   (size_t)(columns > 1 ? columns - 1 : 0), 0);
		rc = morph_buf_printf(&frame, "%s%s%s\n", cli_color_enabled(view->ctx) ? style : "",
				      clipped, cli_color_enabled(view->ctx) ? ANSI_RESET : "");
	}
	if (!rc)
		rc = morph_tui_frame_present(&view->frame, stdout, columns, rows,
					     morph_buf_cstr(&frame));
	morph_buf_cleanup(&frame);
	morph_buf_cleanup(&line);
	return rc;
}

static int jobs_update(void *data, const struct morph_tui_event *event)
{
	struct jobs_view *view = data;
	if (event->type == MORPH_TUI_EVENT_TICK) {
		/* Yield the terminal to permission/interaction dialogs on the owner. */
		if (cli_ui_owner_call_pending(view->ctx))
			return MORPH_TUI_SCREEN_CLOSE;
		if (jobs_now() - view->refreshed < 1000)
			return MORPH_TUI_SCREEN_CONTINUE;
		int rc = jobs_refresh(view);
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
						      job_string(job, "session_id"));
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
		(void)morph_tui_list_update(&view->selection, key);
	}
	int rc = jobs_refresh(view);
	return rc ? rc : MORPH_TUI_SCREEN_REDRAW;
}

int cli_jobs_view(struct cli_context *ctx, const char *id)
{
	struct jobs_view view = {.ctx = ctx};
	struct morph_tui_screen screen = {0};
	const struct morph_tui_screen_ops ops = {.draw = jobs_draw, .update = jobs_update};
	int rc = morph_buf_init(&view.document, BUFSIZ);
	if (!rc)
		rc = jobs_refresh(&view);
	if (!rc && id) {
		cJSON *details = NULL;
		rc = runtime_process_read(ctx->runtime, id, &details);
		if (!rc) {
			cJSON *job;
			size_t index = 0;
			cJSON_ArrayForEach(job, view.jobs)
			{
				if (!strcmp(job_string(job, "session_id"),
					    job_string(details, "session_id"))) {
					view.selection.index = index;
					view.detail = 1;
					break;
				}
				index++;
			}
			if (!view.detail)
				rc = -ENOENT;
			else
				rc = jobs_refresh(&view);
		}
		cJSON_Delete(details);
	}
	screen.interrupted = &ctx->interrupted;
	if (!rc)
		rc = morph_tui_screen_open(&screen, stdin, stdout,
					   MORPH_TUI_SESSION_ALTERNATE | MORPH_TUI_SESSION_PASTE);
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
	morph_tui_text_view_cleanup(&view.lines);
	return rc;
}
