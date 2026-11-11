#include "sapi/cli/widgets/feed.h"
#include "util/terminal_size.h"
#include <morph/tui/terminal.h>
#include "sapi/cli/pages/turn_details.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/services/command_job.h"
#include "sapi/cli/widgets/transcript.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/pages/jobs.h"
#include "sapi/cli/services/agents.h"


#define CLI_FEED_TERMINAL_DEFAULT_COLUMNS 80
#define CLI_FEED_TERMINAL_FRAME_MS 80
#define CLI_FEED_TERMINAL_ANIMATION_MS 32
#define CLI_FEED_TERMINAL_WORK_FRAME_MS 1000
#define CLI_FEED_TERMINAL_STATUS_MAX 2000
#define CLI_FEED_TERMINAL_QUEUE_VISIBLE 3
#define CLI_FEED_PREVIEW_RESERVED_ROWS 12

enum cli_feed_work_state {
	CLI_FEED_TERMINAL_WORK_NONE,
	CLI_FEED_TERMINAL_WORK_RUNNING,
	CLI_FEED_TERMINAL_WORK_COMPLETED,
	CLI_FEED_TERMINAL_WORK_STOPPED,
	CLI_FEED_TERMINAL_WORK_FAILED,
};

struct cli_feed {
	int hidden;
	FILE *output;
	int output_fd;
	morph_buf_t live_text;
	morph_buf_t markdown_preview;
	morph_buf_t agent_summary;
	int markdown_preview_prefix;
	int live_active;
	int live_independent;
	int live_reasoning;
	int64_t live_started_ms;
	int live_visible;
	int live_anchored;
	int live_rows;
	int job_rows;
	int is_terminal;
	int transient;
	int update_active;
	int columns;
	int64_t next_frame_ms;
	int64_t next_content_ms;
	int content_dirty;
	int work_rows_below;
	int work_finalizing;
	enum cli_feed_work_state work_state;
	int64_t work_started_ms;
	int64_t work_elapsed_ms;
	time_t work_finished_at;
};

int cli_feed_update_active(const struct cli_application *ctx)
{
	return ctx && ctx->terminal && ctx->terminal->update_active;
}

void cli_feed_update_begin(struct cli_application *ctx)
{
	struct cli_feed *terminal = ctx ? ctx->terminal : NULL;

	if (!terminal || terminal->hidden || !terminal->transient || terminal->update_active)
		return;
	fprintf(terminal->output, "\033[?2026h");
	terminal->update_active = 1;
}

void cli_feed_update_end(struct cli_application *ctx)
{
	if (!cli_feed_update_active(ctx))
		return;
	fprintf(ctx->terminal->output, "\033[?2026l");
	fflush(ctx->terminal->output);
	ctx->terminal->update_active = 0;
}

static int64_t cli_feed_now_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void cli_feed_turn_begin(struct cli_application *ctx)
{
	if (!ctx || !ctx->terminal || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	ctx->terminal->work_state = CLI_FEED_TERMINAL_WORK_RUNNING;
	ctx->terminal->work_started_ms = cli_feed_now_ms();
	ctx->terminal->work_elapsed_ms = 0;
	ctx->terminal->content_dirty = 1;
}

static void cli_feed_work_label(struct cli_feed *terminal, int64_t elapsed)
{
	const char label[] = "Working";
	int64_t phase = elapsed > 600 ? (elapsed - 600) % 4000 : 0;
	double position = (double)(phase < 1000 ? phase : 1000) / 1000.0 * 13.0 - 3.0;

	fprintf(terminal->output, ANSI_RESET "  ");
	for (size_t i = 0; i < sizeof(label) - 1; i++) {
		double distance = ((double)i + 0.5 - position) / 3.0;

		if (distance < 0.0)
			distance = -distance;
		double intensity = distance < 1.0 ? 1.0 - distance : 0.0;
		int shade = 138 + (int)(117.0 * intensity * intensity * (3.0 - 2.0 * intensity));

		fprintf(terminal->output, "\033[38;2;%d;%d;%dm%c", shade, shade, shade, label[i]);
	}
	fprintf(terminal->output, ANSI_RESET);
}

static int cli_feed_work_interval(const struct cli_feed *terminal, int64_t now)
{
	int64_t elapsed = now - terminal->work_started_ms;
	int64_t phase = elapsed > 600 ? (elapsed - 600) % 4000 : 0;
	int64_t delay = elapsed < 600  ? 600 - elapsed
			: phase < 1000 ? CLI_FEED_TERMINAL_ANIMATION_MS
				       : 4000 - phase;
	int64_t second = 1000 - elapsed % 1000;

	/* Between sweeps, wake only for the clock or the next sweep. */
	return (int)(delay < second ? delay : second);
}

static void cli_feed_work_row(struct cli_feed *terminal, int columns)
{
	morph_buf_t text;
	char clipped[BUFSIZ];
	int64_t elapsed = terminal->work_state == CLI_FEED_TERMINAL_WORK_RUNNING
				  ? cli_feed_now_ms() - terminal->work_started_ms
				  : terminal->work_elapsed_ms;
	int64_t seconds = elapsed > 0 ? elapsed / 1000 : 0;
	struct tm finished;
	char clock[32] = "";

	if (morph_buf_init(&text, 128) != 0)
		return;
	(void)morph_buf_puts(
		&text, terminal->work_state == CLI_FEED_TERMINAL_WORK_RUNNING	  ? ""
		       : terminal->work_state == CLI_FEED_TERMINAL_WORK_COMPLETED ? "Worked for "
		       : terminal->work_state == CLI_FEED_TERMINAL_WORK_STOPPED ? "Stopped after "
										: "Failed after ");
	if (seconds >= 60)
		(void)morph_buf_printf(&text, "%lldm ", (long long)(seconds / 60));
	(void)morph_buf_printf(&text, "%llds", (long long)(seconds % 60));
	if (terminal->work_state == CLI_FEED_TERMINAL_WORK_RUNNING) {
		(void)morph_buf_puts(&text, terminal->agent_summary.len && columns < 76
				      ? " · esc" : " · esc to interrupt");
		if (terminal->agent_summary.len)
			(void)morph_buf_printf(&text, " · %s",
					       morph_buf_cstr_borrow(&terminal->agent_summary));
	} else if (localtime_r(&terminal->work_finished_at, &finished)) {
		(void)strftime(clock, sizeof(clock), "%H:%M", &finished);
		(void)morph_buf_printf(&text, " · %s", clock);
	}
	int running = terminal->work_state == CLI_FEED_TERMINAL_WORK_RUNNING;
	int prefix = running ? 12 : 2;
	const char *hint = columns < 76 ? " · Ctrl+T" : " · Ctrl+T transcript";
	int hint_width = running && columns >= 45 ? (int)utf8_display_width(hint) : 0;

	(void)utf8_copy_ellipsized_display_width(
		clipped, sizeof(clipped), morph_buf_cstr_borrow(&text),
		(size_t)(columns > prefix + hint_width ? columns - prefix - hint_width : 1), 0);
	if (terminal->work_state == CLI_FEED_TERMINAL_WORK_RUNNING) {
		cli_feed_work_label(terminal, elapsed);
		fprintf(terminal->output, ANSI_DIM " · ");
	} else {
		fprintf(terminal->output, "  ");
	}
	fprintf(terminal->output, ANSI_DIM "%s" ANSI_RESET, clipped);
	if (hint_width)
		fprintf(terminal->output, CLI_COMMAND_COLOR "%s" ANSI_RESET, hint);
	morph_buf_cleanup(&text);
}

void cli_feed_turn_end(struct cli_application *ctx, int result)
{
	struct cli_feed *terminal = ctx ? ctx->terminal : NULL;

	if (!terminal || terminal->work_state != CLI_FEED_TERMINAL_WORK_RUNNING)
		return;
	terminal->work_elapsed_ms = cli_feed_now_ms() - terminal->work_started_ms;
	terminal->work_finished_at = time(NULL);
	terminal->work_state = result >= 0	      ? CLI_FEED_TERMINAL_WORK_COMPLETED
			       : result == -ECANCELED ? CLI_FEED_TERMINAL_WORK_STOPPED
						      : CLI_FEED_TERMINAL_WORK_FAILED;
	/* A finished duration is immutable history, not a live footer. */
	if (terminal->transient) {
		cli_feed_history_begin(ctx);
		if (result < 0 || !cli_presentation_final_visible(ctx))
			fputc('\n', terminal->output);
		cli_feed_work_row(terminal, cli_feed_columns(ctx));
		/* The completed turn owns the gap before the next history block. */
		fprintf(terminal->output, "\n\n");
		fflush(terminal->output);
		terminal->work_state = CLI_FEED_TERMINAL_WORK_NONE;
	}
}

static int cli_feed_output_columns(const struct cli_feed *terminal)
{
	return morph_terminal_columns(terminal ? terminal->output_fd : -1,
				      CLI_FEED_TERMINAL_DEFAULT_COLUMNS);
}

static int cli_feed_output_rows(const struct cli_feed *terminal)
{
	int columns;
	int rows;

	morph_terminal_dimensions(terminal ? terminal->output_fd : -1, &columns, &rows);
	return rows;
}

int cli_feed_columns(const struct cli_application *ctx)
{
	return cli_feed_output_columns(ctx ? ctx->terminal : NULL);
}

static void cli_feed_clear_current(struct cli_feed *terminal)
{
	if (!terminal || !terminal->is_terminal)
		return;
	fprintf(terminal->output, "\r\033[2K");
}

static void cli_feed_clear_frame(struct cli_feed *terminal)
{
	int rows;

	if (!terminal || !terminal->is_terminal || !terminal->live_visible)
		return;
	rows = terminal->live_rows > 0 ? terminal->live_rows : 1;
	if (terminal->live_anchored) {
		cli_feed_clear_current(terminal);
		fprintf(terminal->output, "\033[1A");
	}
	for (int row = 0; row < rows; row++) {
		cli_feed_clear_current(terminal);
		if (row + 1 < rows)
			fprintf(terminal->output, "\033[1A");
	}
}

static void cli_feed_sanitize_line(char *text)
{
	if (!text)
		return;
	utf8_sanitize_inplace(text);
	for (char *cur = text; *cur; cur++) {
		unsigned char ch = (unsigned char)*cur;

		if (ch < 0x20u || ch == 0x7fu)
			*cur = ' ';
	}
}

static int cli_feed_text_changed(const struct cli_feed *terminal, const char *text)
{
	const char *current;

	if (!terminal || !text)
		return 0;
	current = morph_buf_cstr_borrow(&terminal->live_text);
	return !current || strcmp(current, text) != 0;
}

int cli_feed_init(struct cli_application *ctx, FILE *output, int output_fd)
{
	struct cli_feed *terminal;
	int rc;

	if (!ctx)
		return -EINVAL;
	terminal = calloc(1, sizeof(*terminal));
	if (!terminal)
		return -ENOMEM;
	rc = morph_buf_init(&terminal->live_text, BUFSIZ);
	if (rc != 0) {
		free(terminal);
		return rc;
	}
	rc = morph_buf_init(&terminal->markdown_preview, BUFSIZ);
	if (rc != 0) {
		morph_buf_cleanup(&terminal->live_text);
		free(terminal);
		return rc;
	}
	rc = morph_buf_init(&terminal->agent_summary, 128);
	if (rc != 0) {
		morph_buf_cleanup(&terminal->markdown_preview);
		morph_buf_cleanup(&terminal->live_text);
		free(terminal);
		return rc;
	}
	terminal->output = output ? output : stdout;
	terminal->output_fd = output_fd;
	terminal->is_terminal = output_fd >= 0 && isatty(output_fd);
	terminal->transient = terminal->is_terminal && cli_output_color_enabled(ctx);
	terminal->columns = cli_feed_output_columns(terminal);
	ctx->terminal = terminal;
	return 0;
}

void cli_feed_cleanup(struct cli_application *ctx)
{
	struct cli_feed *terminal;

	if (!ctx || !ctx->terminal)
		return;
	terminal = ctx->terminal;
	cli_feed_live_clear(ctx);
	cli_feed_update_end(ctx);
	morph_buf_cleanup(&terminal->live_text);
	morph_buf_cleanup(&terminal->markdown_preview);
	morph_buf_cleanup(&terminal->agent_summary);
	free(terminal);
	ctx->terminal = NULL;
}

static void cli_feed_set_live_state(struct cli_application *ctx, const char *text, int independent,
				    int reasoning)
{
	struct cli_feed *terminal;
	char *clean;
	int changed;

	if (!ctx || !ctx->terminal || !text || !text[0])
		return;
	terminal = ctx->terminal;
	clean = utf8_dup_clamped(text, CLI_FEED_TERMINAL_STATUS_MAX);
	if (!clean)
		return;
	cli_feed_sanitize_line(clean);
	changed = cli_feed_text_changed(terminal, clean);
	if (changed) {
		morph_buf_reset(&terminal->live_text);
		if (morph_buf_puts(&terminal->live_text, clean) != 0) {
			free(clean);
			return;
		}
	}
	if (independent && (!terminal->live_independent || !terminal->live_active || changed))
		terminal->live_started_ms = cli_feed_now_ms();
	terminal->live_independent = independent;
	terminal->live_reasoning = reasoning;
	terminal->live_active = 1;
	/* Coalesce content separately from the lightweight animation frames. */
	int64_t next = cli_feed_now_ms() + CLI_FEED_TERMINAL_FRAME_MS;

	terminal->content_dirty = 1;
	if (terminal->next_content_ms > next)
		terminal->next_content_ms = next;
	if (terminal->next_frame_ms > next)
		terminal->next_frame_ms = next;
	if (!terminal->transient) {
		if (changed) {
			fprintf(terminal->output, CLI_FEED_RUNNING " %s\n", clean);
			fflush(terminal->output);
		}
		free(clean);
		return;
	}
	free(clean);
	cli_feed_render_frame(ctx, !terminal->live_visible);
}

void cli_feed_live_set(struct cli_application *ctx, const char *text)
{
	cli_feed_set_live_state(ctx, text, 0, 0);
}

void cli_feed_reasoning_set(struct cli_application *ctx, const char *text)
{
	cli_feed_set_live_state(ctx, text, 0, 1);
}

void cli_feed_activity_set(struct cli_application *ctx, const char *text)
{
	cli_feed_set_live_state(ctx, text, 1, 0);
}

int64_t cli_feed_activity_elapsed_ms(const struct cli_application *ctx)
{
	const struct cli_feed *terminal = ctx ? ctx->terminal : NULL;

	if (!terminal || !terminal->live_active || !terminal->live_independent)
		return -1;
	int64_t elapsed = cli_feed_now_ms() - terminal->live_started_ms;

	return elapsed > 0 ? elapsed : 0;
}

void cli_feed_live_clear(struct cli_application *ctx)
{
	struct cli_feed *terminal;

	if (!ctx || !ctx->terminal)
		return;
	terminal = ctx->terminal;
	if (!cli_turn_details_is_visible(ctx) && terminal->transient && terminal->live_visible) {
		cli_feed_clear_frame(terminal);
		fflush(terminal->output);
	}
	terminal->live_active = 0;
	terminal->live_independent = 0;
	terminal->live_reasoning = 0;
	terminal->live_started_ms = 0;
	terminal->live_visible = 0;
	terminal->live_anchored = 0;
	terminal->live_rows = 0;
	terminal->next_frame_ms = 0;
	morph_buf_reset(&terminal->live_text);
}

void cli_feed_queue_changed(struct cli_application *ctx)
{
	struct cli_feed *terminal;

	if (!ctx || !ctx->terminal)
		return;
	terminal = ctx->terminal;
	terminal->next_frame_ms = 0;
	if (terminal->transient && !cli_turn_details_is_visible(ctx))
		cli_feed_render_frame(ctx, 1);
}

/* cursor_rows is the cursor's distance below the start of the composer.
 * Keep Readline, queued input and tool output untouched on animation ticks. */
int cli_feed_render_tick(struct cli_application *ctx, int cursor_rows)
{
	struct cli_feed *terminal = ctx ? ctx->terminal : NULL;
	int64_t now = cli_feed_now_ms();
	int distance;

	if (!terminal || terminal->hidden || !terminal->transient || !terminal->live_visible ||
	    cli_turn_details_is_visible(ctx) ||
	    terminal->work_state != CLI_FEED_TERMINAL_WORK_RUNNING || terminal->columns < 20 ||
	    cli_feed_output_columns(terminal) != terminal->columns)
		return 0;
	if ((terminal->content_dirty || terminal->live_independent || cli_jobs_visible(ctx) ||
	     terminal->job_rows) &&
	    now >= terminal->next_content_ms)
		return 0;
	if (now < terminal->next_frame_ms)
		return 1;
	terminal->next_frame_ms = now + cli_feed_work_interval(terminal, now);
	distance = terminal->work_rows_below + cursor_rows + terminal->live_anchored;
	/* A long wrapped draft can push the status row out of the viewport. */
	if (distance >= cli_feed_output_rows(terminal))
		return 1;
	int updating = cli_feed_update_active(ctx);

	cli_feed_update_begin(ctx);
	fprintf(terminal->output, "\0337\r");
	if (distance > 0)
		fprintf(terminal->output, "\033[%dA", distance);
	cli_feed_work_row(terminal, terminal->columns);
	fprintf(terminal->output, "\0338");
	if (!updating)
		cli_feed_update_end(ctx);
	return 1;
}

int cli_feed_can_preview(const struct cli_application *ctx)
{
	return ctx && ctx->terminal && ctx->terminal->transient;
}

int cli_feed_markdown_preview_set(struct cli_application *ctx, const char *text, size_t len,
				  int prefix)
{
	struct cli_feed *terminal = ctx ? ctx->terminal : NULL;
	int rc;

	if (!terminal || (!text && len > 0u))
		return -EINVAL;
	morph_buf_reset(&terminal->markdown_preview);
	rc = morph_buf_append(&terminal->markdown_preview, text, len);
	if (rc != 0)
		return rc;
	terminal->markdown_preview_prefix = prefix;
	terminal->content_dirty = 1;
	terminal->next_content_ms = 0;
	terminal->next_frame_ms = 0;
	return 0;
}

static int cli_feed_render_markdown_preview(struct cli_application *ctx, int reserved)
{
	struct cli_feed *terminal = ctx->terminal;
	const char *text = morph_buf_cstr_borrow(&terminal->markdown_preview);
	const char *cursor;
	int rows = 1;
	int limit;
	int gap;

	if (!terminal->markdown_preview.len)
		return 0;
	limit = cli_feed_output_rows(terminal) - reserved;
	if (limit < 1)
		return 0;
	for (cursor = text; *cursor; cursor++)
		if (*cursor == '\n')
			rows++;
	/* Keep every mutable row inside the viewport; committed history above it
	 * must never be erased when a paragraph grows beyond one screen. */
	while (rows > limit) {
		text = strchr(text, '\n') + 1;
		rows--;
	}
	gap = terminal->markdown_preview_prefix && ctx->output.trailing_newlines < 2;
	if (gap)
		fputc('\n', terminal->output);
	if (terminal->markdown_preview_prefix) {
		if (text != morph_buf_cstr_borrow(&terminal->markdown_preview) && text[0] == ' ' &&
		    text[1] == ' ')
			text += 2;
		fprintf(terminal->output, ANSI_BOLD "●" ANSI_RESET " ");
	}
	fprintf(terminal->output, "%s" ANSI_RESET, text);
	return rows + gap;
}

void cli_feed_render_frame(struct cli_application *ctx, int force)
{
	struct cli_feed *terminal;
	const char *text;
	char status_clipped[BUFSIZ];
	char queue_clipped[BUFSIZ];
	morph_buf_t tool_text = {0};
	morph_buf_t tool_body = {0};
	morph_buf_t job_text = {0};
	int job_rows = 0;
	char *queue_items[CLI_FEED_TERMINAL_QUEUE_VISIBLE] = {0};
	size_t queue_total = 0;
	size_t queue_shown = 0;
	int64_t now;
	int columns;
	int budget;
	int tool_live = 0;
	int status_gap = 0;
	int body_rows = 0;
	int status_visible;
	int preview_rows = 0;
	int preview_reserved;

	if (!ctx || !ctx->terminal || ctx->terminal->hidden || cli_turn_details_is_visible(ctx))
		return;
	terminal = ctx->terminal;
	if (!terminal->transient)
		return;
	status_visible = terminal->live_active;
	now = cli_feed_now_ms();
	columns = cli_feed_output_columns(terminal);
	if (columns != terminal->columns) {
		terminal->columns = columns;
		force = 1;
	}
	if (!force && terminal->live_visible && now < terminal->next_frame_ms)
		return;
	if (!force && !terminal->live_anchored && cli_feed_render_tick(ctx, 0))
		return;
	if (ctx->input_job &&
	    cli_command_job_prompts_copy(ctx->input_job, queue_items,
					 CLI_FEED_TERMINAL_QUEUE_VISIBLE, &queue_total) == 0) {
		queue_shown = queue_total < CLI_FEED_TERMINAL_QUEUE_VISIBLE
				      ? queue_total
				      : CLI_FEED_TERMINAL_QUEUE_VISIBLE;
	}
	/* Final output already owns its trailing separator. Match the completed
	 * work row even if persistence keeps the worker alive for another tick. */
	terminal->work_finalizing = cli_presentation_final_visible(ctx) &&
				    terminal->work_state == CLI_FEED_TERMINAL_WORK_RUNNING &&
				    !status_visible && !queue_shown;
	if (!status_visible && !terminal->markdown_preview.len && queue_shown == 0 &&
	    !terminal->work_state && !cli_jobs_visible(ctx) && !cli_agents_visible(ctx)) {
		if (terminal->live_visible) {
			cli_feed_clear_frame(terminal);
			fflush(terminal->output);
			terminal->live_visible = 0;
			terminal->live_anchored = 0;
			terminal->live_rows = 0;
		}
		terminal->job_rows = 0;
		return;
	}
	/* Below the layout minimum, keep one bounded row and keep accepting input. */
	if (columns < 20) {
		if (terminal->live_visible)
			cli_feed_clear_frame(terminal);
		else
			cli_feed_clear_current(terminal);
		(void)utf8_copy_ellipsized_display_width(status_clipped, sizeof(status_clipped),
							 "Widen terminal", (size_t)(columns - 1),
							 0);
		fprintf(terminal->output, ANSI_DIM "%s" ANSI_RESET, status_clipped);
		fflush(terminal->output);
		for (size_t i = 0; i < queue_shown; i++)
			free(queue_items[i]);
		terminal->live_visible = 1;
		terminal->live_anchored = 0;
		terminal->live_rows = 1;
		terminal->next_frame_ms = now + CLI_FEED_TERMINAL_WORK_FRAME_MS;
		return;
	}
	morph_buf_reset(&terminal->agent_summary);
	if (morph_buf_init(&job_text, 128) == 0) {
		int max_jobs = 3;
		if (cli_feed_output_rows(terminal) < 20)
			max_jobs = 1;
		job_rows = cli_jobs_render(ctx, &job_text, columns, max_jobs,
					   cli_output_color_enabled(ctx));
		if (cli_agents_visible(ctx))
			job_rows += cli_agents_render(ctx, &job_text, &terminal->agent_summary,
						     columns, max_jobs, cli_output_color_enabled(ctx));
	}
	text = morph_buf_cstr_borrow(&terminal->live_text);
	if (status_visible && morph_buf_init(&tool_text, 128) == 0) {
		if (terminal->live_independent) {
			char elapsed[32];
			int length =
				snprintf(elapsed, sizeof(elapsed), "%llds",
					 (long long)(cli_feed_activity_elapsed_ms(ctx) / 1000));
			int available = terminal->columns - 6 - length;

			(void)utf8_copy_ellipsized_display_width(
				status_clipped, sizeof(status_clipped), text,
				(size_t)(available > 0 ? available : 1), 0);
			(void)morph_buf_printf(&tool_text, "%s  %s", status_clipped, elapsed);
			text = morph_buf_cstr_borrow(&tool_text);
		} else if (!terminal->live_reasoning) {
			tool_live = cli_transcript_live_text(ctx->transcript, &tool_text, columns,
							     cli_output_color_enabled(ctx));
			if (tool_live)
				text = morph_buf_cstr_borrow(&tool_text);
		}
	}
	budget = terminal->columns - 4;
	if (budget < 1)
		budget = 1;
	(void)utf8_copy_sanitized_display_width(status_clipped, sizeof(status_clipped),
						text ? text : "", (size_t)budget);
	if (terminal->live_visible)
		cli_feed_clear_frame(terminal);
	else
		cli_feed_clear_current(terminal);
	preview_reserved = ctx->input_status.composer_rows + 6;
	if (preview_reserved < CLI_FEED_PREVIEW_RESERVED_ROWS)
		preview_reserved = CLI_FEED_PREVIEW_RESERVED_ROWS;
	preview_rows = cli_feed_render_markdown_preview(ctx, preview_reserved + job_rows +
								     (int)queue_shown);
	if (preview_rows && (status_visible || job_rows || terminal->work_state || queue_shown))
		fputc('\n', terminal->output);
	if (status_visible) {
		/* Match the committed reasoning line from its first streaming frame. */
		status_gap = (tool_live || terminal->live_reasoning) &&
			     ctx->output.trailing_newlines < 2;
		if (status_gap)
			fputc('\n', terminal->output);
		if (tool_live)
			fprintf(terminal->output, "%s", text);
		else if (terminal->live_reasoning)
			fprintf(terminal->output, ANSI_DIM "\033[3m%s" ANSI_RESET, status_clipped);
		else
			fprintf(terminal->output, ANSI_CYAN "%s" ANSI_RESET " %s%s" ANSI_RESET,
				CLI_FEED_RUNNING, terminal->live_independent ? ANSI_DIM : "",
				status_clipped);
		if (tool_live && morph_buf_init(&tool_body, 128) == 0) {
			body_rows = cli_transcript_live_body(ctx->transcript, &tool_body, columns,
							     cli_output_color_enabled(ctx));
			if (body_rows)
				fprintf(terminal->output, "\n%s",
					morph_buf_cstr_borrow(&tool_body));
		}
		if (queue_shown || terminal->work_state)
			fputc('\n', terminal->output);
	}
	if (job_rows) {
		if (status_visible && !queue_shown && !terminal->work_state)
			fputc('\n', terminal->output);
		fprintf(terminal->output, "%s", morph_buf_cstr_borrow(&job_text));
		if (terminal->work_state || queue_shown)
			fputc('\n', terminal->output);
	}
	if (terminal->work_state) {
		if (!terminal->work_finalizing)
			fputc('\n', terminal->output);
		cli_feed_work_row(terminal, terminal->columns);
		if (queue_shown)
			fputc('\n', terminal->output);
	}
	if (queue_shown) {
		const char *heading = terminal->columns >= 44 ? "Messages queued for the next step"
							      : "Messages queued";

		if (status_visible || terminal->work_state)
			fputc('\n', terminal->output);
		fprintf(terminal->output,
			"  " ANSI_DIM CLI_FEED_PENDING ANSI_RESET " \033[38;5;252m%s" ANSI_RESET,
			heading);
		if ((size_t)terminal->columns >= utf8_display_width(heading) + 25)
			fprintf(terminal->output, ANSI_DIM " (Esc to apply now)" ANSI_RESET);
	}
	for (size_t i = 0; i < queue_shown; i++) {
		cli_feed_sanitize_line(queue_items[i]);
		(void)utf8_copy_ellipsized_display_width(queue_clipped, sizeof(queue_clipped),
							 queue_items[i],
							 (size_t)(terminal->columns - 6), 0);
		fprintf(terminal->output, "\n    " ANSI_DIM "↳ %s" ANSI_RESET, queue_clipped);
		free(queue_items[i]);
	}
	if (queue_total > queue_shown)
		fprintf(terminal->output, "\n    " ANSI_DIM "+%zu more queued" ANSI_RESET,
			queue_total - queue_shown);
	morph_buf_cleanup(&tool_text);
	morph_buf_cleanup(&tool_body);
	morph_buf_cleanup(&job_text);
	fflush(terminal->output);
	terminal->live_visible = 1;
	terminal->live_anchored = 0;
	terminal->job_rows = job_rows;
	terminal->live_rows = preview_rows + job_rows + status_visible + status_gap + body_rows +
			      (terminal->work_state ? (terminal->work_finalizing ? 1 : 2) : 0) +
			      (int)queue_shown + (queue_shown > 0) + (queue_total > queue_shown) +
			      (queue_shown > 0 && (status_visible || terminal->work_state));
	terminal->work_rows_below =
		queue_shown ? 2 + (int)queue_shown + (queue_total > queue_shown) : 0;
	terminal->content_dirty = 0;
	terminal->next_content_ms = now + CLI_FEED_TERMINAL_WORK_FRAME_MS;
	terminal->next_frame_ms = now + (terminal->work_state == CLI_FEED_TERMINAL_WORK_RUNNING
						 ? cli_feed_work_interval(terminal, now)
						 : CLI_FEED_TERMINAL_WORK_FRAME_MS);
}

int cli_feed_next_frame_ms(const struct cli_application *ctx)
{
	const struct cli_feed *terminal;
	int64_t remaining;

	if (!ctx || !ctx->terminal)
		return -1;
	terminal = ctx->terminal;
	if (!terminal->transient || (!terminal->live_active && !terminal->markdown_preview.len &&
				     terminal->work_state != CLI_FEED_TERMINAL_WORK_RUNNING &&
				     !cli_jobs_visible(ctx) && !cli_agents_visible(ctx) && !terminal->job_rows))
		return -1;
	if (!terminal->live_visible)
		return 0;
	remaining = terminal->next_frame_ms - cli_feed_now_ms();
	if (remaining <= 0)
		return 0;
	return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

void cli_feed_composer_suspend(struct cli_application *ctx)
{
	struct cli_feed *terminal;

	if (!ctx || !ctx->terminal)
		return;
	terminal = ctx->terminal;
	if (!terminal->is_terminal)
		return;
	cli_feed_clear_current(terminal);
	while (ctx->input_status.padding_rows > 0) {
		fprintf(terminal->output, "\033[1A\r\033[2K");
		ctx->input_status.padding_rows--;
	}
	cli_feed_clear_frame(terminal);
	terminal->live_visible = 0;
	terminal->live_anchored = 0;
	terminal->live_rows = 0;
	fflush(terminal->output);
}

void cli_feed_composer_resume(struct cli_application *ctx)
{
	struct cli_feed *terminal;

	if (!ctx || !ctx->terminal)
		return;
	terminal = ctx->terminal;
	if (!terminal->transient)
		return;
	if (!terminal->live_visible)
		cli_feed_render_frame(ctx, 1);
	if (terminal->live_visible && !terminal->live_anchored) {
		fputc('\n', terminal->output);
		fflush(terminal->output);
		terminal->live_anchored = 1;
	}
	if (ctx->input_status.enabled && !ctx->input_status.padding_rows) {
		/* Reserve the same gap for live and committed status rows. */
		int padding = terminal->live_visible ? 2 : 1;

		if (padding == 2)
			fprintf(terminal->output, ANSI_RESET "\033[2K\n");
		fprintf(terminal->output, CLI_INPUT_BACKGROUND "\033[2K\n" ANSI_RESET);
		ctx->input_status.padding_rows = padding;
	}
}

void cli_feed_history_begin(struct cli_application *ctx)
{
	struct cli_feed *terminal;

	if (!ctx || !ctx->terminal || cli_turn_details_is_visible(ctx))
		return;
	terminal = ctx->terminal;
	if (!terminal->transient || !terminal->live_visible)
		return;
	cli_feed_clear_frame(terminal);
	terminal->live_visible = 0;
	terminal->live_anchored = 0;
	terminal->live_rows = 0;
	fflush(terminal->output);
}

void cli_feed_history_end(struct cli_application *ctx)
{
	if (!ctx || !ctx->terminal || !ctx->terminal->live_active)
		return;
	cli_feed_render_frame(ctx, 1);
}

void cli_feed_resize(struct cli_application *ctx)
{
	if (!ctx || !ctx->terminal)
		return;
	ctx->terminal->columns = cli_feed_output_columns(ctx->terminal);
	ctx->terminal->next_frame_ms = 0;
}

void cli_feed_set_hidden(struct cli_application *ctx, int hidden)
{
	if (ctx && ctx->terminal)
		ctx->terminal->hidden = hidden != 0;
}

int cli_feed_live_active(const struct cli_application *ctx)
{
	return ctx && ctx->terminal && ctx->terminal->live_active;
}
