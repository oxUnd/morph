#include "sapi/cli/presentation/feed.h"
#include <nb/err.h>
#include "sapi/cli/application.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/models/jobs.h"
#include "sapi/cli/presentation/work_status.h"
#include "sapi/cli/presentation/style.h"
#include <nothing/tui/terminal/size.h>
#include <nb/cleanup.h>
#include <nb/utf8.h>
#include <nothing/tui/terminal/text.h>
#include <nothing/tui/terminal.h>
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/services/command_job.h"
#include "sapi/cli/presentation/transcript.h"
#include "sapi/cli/presentation/agents.h"
#include "sapi/cli/presentation/jobs.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/models/agents.h"
#include <nb/time.h>

#define CLI_FEED_TERMINAL_DEFAULT_COLUMNS 80
#define CLI_FEED_TERMINAL_FRAME_MS 80
#define CLI_FEED_TERMINAL_WORK_FRAME_MS 1000
#define CLI_FEED_TERMINAL_STATUS_MAX 2000
#define CLI_FEED_TERMINAL_QUEUE_VISIBLE 3
#define CLI_FEED_PREVIEW_RESERVED_ROWS 12

struct cli_feed {
	int hidden;
	FILE *output;
	int output_fd;
	nb_buf live_text;
	nb_buf markdown_preview;
	nb_buf agent_summary;
	int markdown_preview_prefix;
	int live_active;
	int live_independent;
	int live_reasoning;
	int64_t live_started_ms;
	int live_visible;
	int live_rows;
	int job_rows;
	int background_rows;
	int background_rows_below;
	int is_terminal;
	int transient;
	int update_active;
	int columns;
	int64_t next_frame_ms;
	int64_t next_content_ms;
	int content_dirty;
	int work_rows_below;
	int work_active;
	int64_t work_started_ms;
};

int cli_feed_update_active(const struct cli_application *ctx)
{
	return ctx && ctx->terminal && ctx->terminal->update_active;
}

void cli_feed_update_begin(struct cli_application *ctx)
{
	if (cli_chat_active(ctx))
		return;

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
	int64_t now = nb_time_monotonic_ns();

	return now > 0 ? now / 1000000 : 0;
}

void cli_feed_turn_begin(struct cli_application *ctx)
{
	if (cli_chat_active(ctx)) {
		cli_chat_turn_begin(ctx);
		return;
	}

	if (!ctx || !ctx->terminal || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;

	ctx->terminal->work_active = 1;
	ctx->terminal->work_started_ms = cli_feed_now_ms();
	ctx->terminal->content_dirty = 1;
}

static void cli_feed_work_row(struct cli_feed *terminal, int64_t now)
{
	nb_buf row NB_CLEANUP(nb_buf_cleanup) = {0};
	if (cli_work_status_running(&row, terminal->columns, 1, now - terminal->work_started_ms,
				    now, nb_buf_cstr(&terminal->agent_summary)) == 0)
		fputs(nb_buf_cstr(&row), terminal->output);
}

void cli_feed_turn_end(struct cli_application *ctx, int result)
{
	if (cli_chat_active(ctx)) {
		cli_chat_turn_end(ctx, result);
		return;
	}

	struct cli_feed *terminal = ctx ? ctx->terminal : NULL;

	if (!terminal || !terminal->work_active)
		return;

	terminal->work_active = 0;
	if (terminal->transient) {
		nb_buf row NB_CLEANUP(nb_buf_cleanup) = {0};
		int64_t elapsed = cli_feed_now_ms() - terminal->work_started_ms;
		cli_feed_history_begin(ctx);
		if (result < 0 || !cli_presentation_final_visible(ctx))
			fputc('\n', terminal->output);

		if (cli_work_status_completed(&row, cli_feed_columns(ctx), 1, result, elapsed,
					      time(NULL)) == 0)
			fprintf(terminal->output, "  %s", nb_buf_cstr(&row));

		fprintf(terminal->output, "\n\n");
		fflush(terminal->output);
	}
}

static int cli_feed_output_columns(const struct cli_feed *terminal)
{
	return nt_terminal_columns(terminal ? terminal->output_fd : -1,
				      CLI_FEED_TERMINAL_DEFAULT_COLUMNS);
}

static int cli_feed_output_rows(const struct cli_feed *terminal)
{
	int columns;
	int rows;

	nt_terminal_dimensions(terminal ? terminal->output_fd : -1, &columns, &rows);
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

	nb_utf8_sanitize_inplace(text);
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

	current = nb_buf_cstr(&terminal->live_text);
	return !current || strcmp(current, text) != 0;
}

int cli_feed_init(struct cli_application *ctx, FILE *output, int output_fd)
{
	struct cli_feed *terminal;
	int rc;

	if (!ctx)
		return NB_ERR_INVAL;

	terminal = calloc(1, sizeof(*terminal));
	if (!terminal)
		return NB_ERR_NOMEM;

	rc = (nb_buf_init(&terminal->live_text),
	      nb_buf_reserve(&terminal->live_text, BUFSIZ) ? 0 : NB_ERR_NOMEM);
	if (rc != 0) {
		free(terminal);
		return rc;
	}

	rc = (nb_buf_init(&terminal->markdown_preview),
	      nb_buf_reserve(&terminal->markdown_preview, BUFSIZ) ? 0 : NB_ERR_NOMEM);
	if (rc != 0) {
		nb_buf_cleanup(&terminal->live_text);
		free(terminal);
		return rc;
	}

	rc = (nb_buf_init(&terminal->agent_summary),
	      nb_buf_reserve(&terminal->agent_summary, 128) ? 0 : NB_ERR_NOMEM);
	if (rc != 0) {
		nb_buf_cleanup(&terminal->markdown_preview);
		nb_buf_cleanup(&terminal->live_text);
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
	nb_buf_cleanup(&terminal->live_text);
	nb_buf_cleanup(&terminal->markdown_preview);
	nb_buf_cleanup(&terminal->agent_summary);
	free(terminal);
	ctx->terminal = NULL;
}

static void cli_feed_set_live_state(struct cli_application *ctx, const char *text, int independent,
				    int reasoning)
{
	if (cli_chat_active(ctx)) {
		cli_chat_activity(ctx, text);
		return;
	}

	struct cli_feed *terminal;
	char *clean;
	int changed;

	if (!ctx || !ctx->terminal || !text || !text[0])
		return;

	terminal = ctx->terminal;
	clean = nb_utf8_ellipsize_bytes(text, CLI_FEED_TERMINAL_STATUS_MAX);
	if (!clean)
		return;

	cli_feed_sanitize_line(clean);
	changed = cli_feed_text_changed(terminal, clean);
	if (changed) {
		nb_buf_clear(&terminal->live_text);
		if ((nb_buf_append_cstr(&terminal->live_text, clean) ? 0 : NB_ERR_NOMEM) != 0) {
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
	if (cli_chat_active(ctx)) {
		cli_chat_activity(ctx, "");
		return;
	}

	struct cli_feed *terminal;

	if (!ctx || !ctx->terminal)
		return;

	terminal = ctx->terminal;
	if (!cli_output_overlay_visible(ctx) && terminal->transient && terminal->live_visible) {
		cli_feed_clear_frame(terminal);
		fflush(terminal->output);
	}

	terminal->live_active = 0;
	terminal->live_independent = 0;
	terminal->live_reasoning = 0;
	terminal->live_started_ms = 0;
	terminal->live_visible = 0;
	terminal->live_rows = 0;
	terminal->next_frame_ms = 0;
	nb_buf_clear(&terminal->live_text);
}

void cli_feed_queue_changed(struct cli_application *ctx)
{
	struct cli_feed *terminal;

	if (!ctx || !ctx->terminal)
		return;

	terminal = ctx->terminal;
	terminal->next_frame_ms = 0;
	if (terminal->transient && !cli_output_overlay_visible(ctx))
		cli_feed_render_frame(ctx, 1);
}

static int cli_feed_agents_visible(const struct cli_application *ctx)
{
	return (ctx->composer.ultra && ctx->turn_active) ||
	       cli_agents_model_active(&ctx->agents);
}

static int cli_feed_render_tick(struct cli_application *ctx, int64_t now, int work_animated)
{
	struct cli_feed *terminal = ctx->terminal;
	int jobs_animated = terminal->background_rows && cli_jobs_model_visible(ctx->jobs);
	if (!terminal->live_visible || terminal->columns < 20 || (!work_animated && !jobs_animated))
		return 0;

	if ((terminal->content_dirty || terminal->live_independent ||
	     cli_jobs_model_visible(ctx->jobs) || terminal->job_rows) &&
	    now >= terminal->next_content_ms)
		return 0;

	int64_t elapsed = now - terminal->work_started_ms;
	terminal->next_frame_ms = now + (work_animated ? cli_work_status_interval_ms(elapsed, now)
						       : cli_style_running_interval_ms(now));
	int updating = cli_feed_update_active(ctx);

	cli_feed_update_begin(ctx);
	fprintf(terminal->output, "\0337");
	if (work_animated) {
		int distance = terminal->work_rows_below;
		if (distance < cli_feed_output_rows(terminal)) {
			fprintf(terminal->output, "\r");
			if (distance > 0)
				fprintf(terminal->output, "\033[%dA", distance);

			cli_feed_work_row(terminal, now);
		}
	}

	if (jobs_animated) {
		int distance = terminal->background_rows_below;
		if (distance < cli_feed_output_rows(terminal)) {
			fprintf(terminal->output, "\0338\0337\r");
			if (distance > 0)
				fprintf(terminal->output, "\033[%dA", distance);

			fprintf(terminal->output, ANSI_RESET "%s" CLI_FEED_RUNNING ANSI_RESET,
				cli_style_running_color(now));
		}
	}

	fprintf(terminal->output, "\0338");
	if (!updating)
		cli_feed_update_end(ctx);

	return 1;
}

int cli_feed_can_preview(const struct cli_application *ctx)
{
	return !cli_chat_active(ctx) && ctx && ctx->terminal && ctx->terminal->transient;
}

int cli_feed_markdown_preview_set(struct cli_application *ctx, const char *text, size_t len,
				  int prefix)
{
	struct cli_feed *terminal = ctx ? ctx->terminal : NULL;
	int rc;

	if (!terminal || (!text && len > 0u))
		return NB_ERR_INVAL;

	nb_buf_clear(&terminal->markdown_preview);
	rc = (nb_buf_append(&terminal->markdown_preview, text, len) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;

	terminal->markdown_preview_prefix = prefix;
	terminal->content_dirty = 1;
	terminal->next_content_ms = 0;
	terminal->next_frame_ms = 0;
	return 0;
}

/* The terminal wraps a logical preview line that exceeds the current width,
 * so the frame height must count wrapped rows as well. */
static int cli_feed_preview_rows(const char *text, int columns, int prefix)
{
	int rows = 0;
	const char *line = text;
	size_t available = columns > 0 ? (size_t)columns : 1u;
	int first = 1;

	while (line) {
		const char *newline = strchr(line, '\n');
		size_t len = newline ? (size_t)(newline - line) : strlen(line);
		char *copy = strndup(line, len);
		size_t width = copy ? nt_terminal_display_width_ansi(copy) : 0;
		size_t line_rows;

		free(copy);
		if (first && prefix)
			width += 2u;

		/* An empty line still occupies one terminal row, and a trailing
		 * newline leaves the cursor on that empty row. */
		line_rows = width > 0 ? (width + available - 1u) / available : 1u;
		rows += (int)line_rows;
		first = 0;
		line = newline ? newline + 1 : NULL;
	}

	return rows;
}

static int cli_feed_render_markdown_preview(struct cli_application *ctx, int reserved, int draw)
{
	struct cli_feed *terminal = ctx->terminal;
	const char *text = nb_buf_cstr(&terminal->markdown_preview);
	const char *original = text;
	const char *visible = text;
	int prefix = terminal->markdown_preview_prefix;
	int rows;
	int limit;
	int gap;

	if (!terminal->markdown_preview.len)
		return 0;

	limit = cli_feed_output_rows(terminal) - reserved;
	if (limit < 1)
		return 0;

	/* Keep every mutable row inside the viewport; committed history above it
	 * must never be erased when a paragraph grows beyond one screen. */
	rows = cli_feed_preview_rows(visible, terminal->columns, prefix);
	while (rows > limit) {
		const char *newline = strchr(visible, '\n');

		if (!newline)
			break;

		text = newline + 1;
		visible = text;
		if (prefix && text != original && visible[0] == ' ' && visible[1] == ' ')
			visible += 2;

		rows = cli_feed_preview_rows(visible, terminal->columns, prefix);
	}

	struct nt_view_insets preview_margin = {.top = 1};

	gap = prefix ? cli_output_block_gap_margin(preview_margin,
						   ctx->output.trailing_newlines)
		     : 0;
	if (!draw)
		return rows + gap;

	for (int i = 0; i < gap; i++)
		fputc('\n', terminal->output);

	if (prefix)
		fprintf(terminal->output, ANSI_BOLD CLI_FEED_MESSAGE ANSI_RESET " ");

	fprintf(terminal->output, "%s" ANSI_RESET, visible);
	return rows + gap;
}

void cli_feed_render_frame(struct cli_application *ctx, int force)
{
	if (cli_chat_active(ctx))
		return;

	struct cli_feed *terminal;
	const char *text;
	char status_clipped[BUFSIZ];
	nb_buf tool_text NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf tool_body NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf job_text NB_CLEANUP(nb_buf_cleanup) = {0};
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
	int waiting;
	int work_visible;
	int preview_rows = 0;
	int preview_reserved;

	if (!ctx || !ctx->terminal || ctx->terminal->hidden || cli_output_overlay_visible(ctx))
		return;

	terminal = ctx->terminal;
	if (!terminal->transient)
		return;

	waiting = ctx->jobs && cli_jobs_model_waiting(ctx->jobs);
	status_visible = terminal->live_active && !waiting;
	work_visible = !waiting && terminal->work_active;
	now = cli_feed_now_ms();
	columns = cli_feed_output_columns(terminal);
	if (columns != terminal->columns) {
		terminal->columns = columns;
		force = 1;
	}

	if (!force && terminal->live_visible && now < terminal->next_frame_ms)
		return;

	if (!force && cli_feed_render_tick(ctx, now, work_visible))
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
	int work_finalizing = work_visible && !status_visible && !queue_shown &&
			      cli_presentation_final_visible(ctx);
	if (!status_visible && !terminal->markdown_preview.len && queue_shown == 0 &&
	    !terminal->work_active && !cli_jobs_model_visible(ctx->jobs) &&
	    !cli_feed_agents_visible(ctx)) {
		if (terminal->live_visible) {
			cli_feed_clear_frame(terminal);
			fflush(terminal->output);
			terminal->live_visible = 0;
			terminal->live_rows = 0;
		}

		terminal->job_rows = 0;
		terminal->background_rows = 0;
		return;
	}

	/* Below the layout minimum, keep one bounded row and keep accepting input. */
	if (columns < 20) {
		if (terminal->live_visible)
			cli_feed_clear_frame(terminal);
		else
			cli_feed_clear_current(terminal);

		(void)nt_terminal_copy_ellipsized_display_width(
			status_clipped, sizeof(status_clipped), "Widen terminal",
			(size_t)(columns - 1), 0);
		fprintf(terminal->output, ANSI_DIM "%s" ANSI_RESET, status_clipped);
		fflush(terminal->output);
		for (size_t i = 0; i < queue_shown; i++)
			free(queue_items[i]);

		terminal->live_visible = 1;
		terminal->live_rows = 1;
		terminal->next_frame_ms = now + CLI_FEED_TERMINAL_WORK_FRAME_MS;
		return;
	}

	nb_buf agent_summary NB_CLEANUP(nb_buf_cleanup) = {0};
	int background_rows = cli_jobs_render(ctx->jobs, &job_text, columns,
					      cli_output_color_enabled(ctx), now);
	int agent_rows = 0;
	if (background_rows >= 0 && cli_feed_agents_visible(ctx)) {
		int maximum = cli_feed_output_rows(terminal) < 20 ? 1 : 3;
		agent_rows = cli_agents_model_render(&ctx->agents, &job_text, &agent_summary,
						     columns, maximum, cli_output_color_enabled(ctx));
	}

	if (background_rows < 0 || agent_rows < 0) {
		for (size_t i = 0; i < queue_shown; i++)
			free(queue_items[i]);

		return;
	}

	terminal->background_rows = background_rows;
	job_rows = background_rows + agent_rows;
	nb_buf_move(&terminal->agent_summary, &agent_summary);

	text = nb_buf_cstr(&terminal->live_text);
	if (status_visible &&
	    (nb_buf_init(&tool_text), nb_buf_reserve(&tool_text, 128) ? 0 : NB_ERR_NOMEM) == 0) {
		if (terminal->live_independent) {
			char elapsed[32];
			int length =
				snprintf(elapsed, sizeof(elapsed), "%llds",
					 (long long)(cli_feed_activity_elapsed_ms(ctx) / 1000));
			int available = terminal->columns - 6 - length;

			(void)nt_terminal_copy_ellipsized_display_width(
				status_clipped, sizeof(status_clipped), text,
				(size_t)(available > 0 ? available : 1), 0);
			(void)nb_buf_printf(&tool_text, "%s  %s", status_clipped, elapsed);
			text = nb_buf_cstr(&tool_text);
		} else if (!terminal->live_reasoning) {
			tool_live = cli_transcript_live_text(ctx->transcript, &tool_text, columns,
							     cli_output_color_enabled(ctx));
			if (tool_live)
				text = nb_buf_cstr(&tool_text);
		}
	}

	budget = terminal->columns - 4;
	if (budget < 1)
		budget = 1;

	(void)nt_terminal_copy_sanitized_display_width(status_clipped, sizeof(status_clipped),
							  text ? text : "", (size_t)budget);
	preview_reserved = CLI_FEED_PREVIEW_RESERVED_ROWS + job_rows + (int)queue_shown;
	preview_rows = cli_feed_render_markdown_preview(ctx, preview_reserved, 0);
	struct nt_view_insets status_margin = {.top = 1};

	status_gap = status_visible && (tool_live || terminal->live_reasoning)
			     ? cli_output_block_gap_margin(status_margin,
							   ctx->output.trailing_newlines)
			     : 0;
	if (status_visible && tool_live &&
	    (nb_buf_init(&tool_body), nb_buf_reserve(&tool_body, 128) ? 0 : NB_ERR_NOMEM) == 0)
		body_rows = cli_transcript_live_body(ctx->transcript, &tool_body, columns,
						     cli_output_color_enabled(ctx));

	int frame_rows = preview_rows + job_rows + status_visible + status_gap + body_rows +
		(work_visible ? (work_finalizing ? 1 : 2) : 0) +
		(int)queue_shown + (queue_shown > 0) + (queue_total > queue_shown) +
		(queue_shown > 0 && (status_visible || work_visible));
	if (terminal->live_visible)
		cli_feed_clear_frame(terminal);
	else
		cli_feed_clear_current(terminal);

	(void)cli_feed_render_markdown_preview(ctx, preview_reserved, 1);
	if (preview_rows && (status_visible || job_rows || work_visible || queue_shown))
		fputc('\n', terminal->output);

	if (status_visible) {
		/* Match the committed reasoning line from its first streaming frame. */
		for (int i = 0; i < status_gap; i++)
			fputc('\n', terminal->output);

		if (tool_live)
			fprintf(terminal->output, "%s", text);
		else if (terminal->live_reasoning)
			fprintf(terminal->output, ANSI_DIM "\033[3m%s" ANSI_RESET, status_clipped);
		else
			fprintf(terminal->output, ANSI_CYAN "%s" ANSI_RESET " %s%s" ANSI_RESET,
				CLI_FEED_RUNNING, terminal->live_independent ? ANSI_DIM : "",
				status_clipped);

		if (body_rows)
			fprintf(terminal->output, "\n%s", nb_buf_cstr(&tool_body));

		if (queue_shown || work_visible)
			fputc('\n', terminal->output);
	}

	if (job_rows) {
		if (status_visible && !queue_shown && !work_visible)
			fputc('\n', terminal->output);

		fprintf(terminal->output, "%s", nb_buf_cstr(&job_text));
		if (work_visible || queue_shown)
			fputc('\n', terminal->output);
	}

	if (work_visible) {
		if (!work_finalizing)
			fputc('\n', terminal->output);

		cli_feed_work_row(terminal, now);
		if (queue_shown)
			fputc('\n', terminal->output);
	}

	if (queue_shown) {
		const char *heading = terminal->columns >= 44 ? "Messages queued for the next step"
							      : "Messages queued";

		if (status_visible || work_visible)
			fputc('\n', terminal->output);

		fprintf(terminal->output,
			ANSI_DIM CLI_FEED_PENDING ANSI_RESET " \033[38;5;252m%s" ANSI_RESET,
			heading);
		if ((size_t)terminal->columns >= nb_utf8_width_cstr(heading) + 25)
			fprintf(terminal->output, ANSI_DIM " (Esc to apply now)" ANSI_RESET);
	}

	nb_buf queue_text NB_CLEANUP(nb_buf_cleanup) = {0};
	int queue_rc = cli_work_status_queue(&queue_text, (const char *const *)queue_items,
		queue_shown, queue_total, terminal->columns, 1);
	if (!queue_rc && queue_text.len)
		fprintf(terminal->output, "\n%s", nb_buf_cstr(&queue_text));

	for (size_t i = 0; i < queue_shown; i++)
		free(queue_items[i]);

	nb_buf_cleanup(&tool_text);
	nb_buf_cleanup(&tool_body);
	nb_buf_cleanup(&job_text);
	fflush(terminal->output);
	terminal->live_visible = 1;
	terminal->job_rows = job_rows;
	terminal->live_rows = frame_rows;
	terminal->work_rows_below =
		queue_shown ? 2 + (int)queue_shown + (queue_total > queue_shown) : 0;
	terminal->background_rows_below = job_rows - 1 +
		(work_visible ? (work_finalizing ? 1 : 2) + terminal->work_rows_below :
		 queue_shown ? 1 + (int)queue_shown + (queue_total > queue_shown) : 0);
	terminal->content_dirty = 0;
	terminal->next_content_ms = now + CLI_FEED_TERMINAL_WORK_FRAME_MS;
	terminal->next_frame_ms = terminal->next_content_ms;
	if (work_visible)
		terminal->next_frame_ms = now +
			cli_work_status_interval_ms(now - terminal->work_started_ms, now);
	else if (terminal->background_rows)
		terminal->next_frame_ms = now + cli_style_running_interval_ms(now);
}

void cli_feed_history_begin(struct cli_application *ctx)
{
	struct cli_feed *terminal;

	if (!ctx || !ctx->terminal || cli_output_overlay_visible(ctx))
		return;

	terminal = ctx->terminal;
	if (!terminal->transient || !terminal->live_visible)
		return;

	cli_feed_clear_frame(terminal);
	terminal->live_visible = 0;
	terminal->live_rows = 0;
	fflush(terminal->output);
}

void cli_feed_history_end(struct cli_application *ctx)
{
	if (!ctx || !ctx->terminal || !ctx->terminal->live_active)
		return;

	cli_feed_render_frame(ctx, 1);
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
