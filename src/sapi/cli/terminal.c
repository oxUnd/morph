#include "sapi/cli/internal.h"
#include "sapi/cli/terminal.h"

#include <sys/ioctl.h>

#define CLI_TERMINAL_DEFAULT_COLUMNS 80
#define CLI_TERMINAL_FRAME_MS 80
#define CLI_TERMINAL_ANIMATION_MS 32
#define CLI_TERMINAL_WORK_FRAME_MS 1000
#define CLI_TERMINAL_STATUS_MAX 2000
#define CLI_TERMINAL_QUEUE_VISIBLE 3

enum cli_work_state {
	CLI_WORK_NONE,
	CLI_WORK_RUNNING,
	CLI_WORK_COMPLETED,
	CLI_WORK_STOPPED,
	CLI_WORK_FAILED,
};

struct cli_terminal {
	FILE *output;
	int output_fd;
	morph_buf_t live_text;
	int live_active;
	int live_independent;
	int live_reasoning;
	int64_t live_started_ms;
	int live_visible;
	int live_anchored;
	int live_rows;
	int is_terminal;
	int transient;
	int update_active;
	int columns;
	int64_t next_frame_ms;
	int64_t next_content_ms;
	int content_dirty;
	int work_rows_below;
	int work_finalizing;
	enum cli_work_state work_state;
	int64_t work_started_ms;
	int64_t work_elapsed_ms;
	time_t work_finished_at;
};

int cli_terminal_update_active(const struct cli_context *ctx)
{
	return ctx && ctx->terminal && ctx->terminal->update_active;
}

void cli_terminal_update_begin(struct cli_context *ctx)
{
	struct cli_terminal *terminal = ctx ? ctx->terminal : NULL;

	if (!terminal || !terminal->transient || terminal->update_active)
		return;
	fprintf(terminal->output, "\033[?2026h");
	terminal->update_active = 1;
}

void cli_terminal_update_end(struct cli_context *ctx)
{
	if (!cli_terminal_update_active(ctx))
		return;
	fprintf(ctx->terminal->output, "\033[?2026l");
	fflush(ctx->terminal->output);
	ctx->terminal->update_active = 0;
}

static int64_t terminal_now_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void cli_terminal_turn_begin(struct cli_context *ctx)
{
	if (!ctx || !ctx->terminal ||
	    ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	ctx->terminal->work_state = CLI_WORK_RUNNING;
	ctx->terminal->work_started_ms = terminal_now_ms();
	ctx->terminal->work_elapsed_ms = 0;
	ctx->terminal->content_dirty = 1;
}

static void terminal_work_label(struct cli_terminal *terminal, int64_t elapsed)
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
		int shade = 138 + (int)(117.0 * intensity * intensity *
			(3.0 - 2.0 * intensity));

		fprintf(terminal->output, "\033[38;2;%d;%d;%dm%c",
			shade, shade, shade, label[i]);
	}
	fprintf(terminal->output, ANSI_RESET);
}

static int terminal_work_interval(const struct cli_terminal *terminal, int64_t now)
{
	int64_t elapsed = now - terminal->work_started_ms;
	int64_t phase = elapsed > 600 ? (elapsed - 600) % 4000 : 0;
	int64_t delay = elapsed < 600 ? 600 - elapsed :
		phase < 1000 ? CLI_TERMINAL_ANIMATION_MS : 4000 - phase;
	int64_t second = 1000 - elapsed % 1000;

	/* Between sweeps, wake only for the clock or the next sweep. */
	return (int)(delay < second ? delay : second);
}

static void terminal_work_row(struct cli_terminal *terminal, int columns)
{
	morph_buf_t text;
	char clipped[BUFSIZ];
	int64_t elapsed = terminal->work_state == CLI_WORK_RUNNING ?
		terminal_now_ms() - terminal->work_started_ms : terminal->work_elapsed_ms;
	int64_t seconds = elapsed > 0 ? elapsed / 1000 : 0;
	struct tm finished;
	char clock[32] = "";

	if (morph_buf_init(&text, 128) != 0)
		return;
	(void)morph_buf_puts(&text, terminal->work_state == CLI_WORK_RUNNING ? "" :
		terminal->work_state == CLI_WORK_COMPLETED ? "Worked for " :
		terminal->work_state == CLI_WORK_STOPPED ? "Stopped after " : "Failed after ");
	if (seconds >= 60)
		(void)morph_buf_printf(&text, "%lldm ", (long long)(seconds / 60));
	(void)morph_buf_printf(&text, "%llds", (long long)(seconds % 60));
	if (terminal->work_state == CLI_WORK_RUNNING)
		(void)morph_buf_puts(&text, " · esc to interrupt");
	else if (localtime_r(&terminal->work_finished_at, &finished)) {
		(void)strftime(clock, sizeof(clock), "%H:%M", &finished);
		(void)morph_buf_printf(&text, " · %s", clock);
	}
	int prefix = terminal->work_state == CLI_WORK_RUNNING ? 12 : 2;

	(void)utf8_copy_ellipsized_display_width(clipped, sizeof(clipped),
		morph_buf_cstr(&text), (size_t)(columns > prefix ? columns - prefix : 1), 0);
	if (terminal->work_state == CLI_WORK_RUNNING) {
		terminal_work_label(terminal, elapsed);
		fprintf(terminal->output, ANSI_DIM " · ");
	} else {
		fprintf(terminal->output, "  ");
	}
	fprintf(terminal->output, ANSI_DIM "%s" ANSI_RESET, clipped);
	morph_buf_cleanup(&text);
}

void cli_terminal_turn_end(struct cli_context *ctx, int result)
{
	struct cli_terminal *terminal = ctx ? ctx->terminal : NULL;

	if (!terminal || terminal->work_state != CLI_WORK_RUNNING)
		return;
	terminal->work_elapsed_ms = terminal_now_ms() - terminal->work_started_ms;
	terminal->work_finished_at = time(NULL);
	terminal->work_state = result >= 0 ? CLI_WORK_COMPLETED :
		result == -ECANCELED ? CLI_WORK_STOPPED : CLI_WORK_FAILED;
	/* A finished duration is immutable history, not a live footer. */
	if (terminal->transient) {
		cli_terminal_history_begin(ctx);
		if (result < 0 || !ctx->final_rendered)
			fputc('\n', terminal->output);
		terminal_work_row(terminal, cli_terminal_columns(ctx));
		fputc('\n', terminal->output);
		fflush(terminal->output);
		terminal->work_state = CLI_WORK_NONE;
	}
}

static int terminal_columns(const struct cli_terminal *terminal)
{
	struct winsize size;
	const char *configured;
	char *end;
	long columns;

	memset(&size, 0, sizeof(size));
	if (terminal && terminal->output_fd >= 0 &&
	    ioctl(terminal->output_fd, TIOCGWINSZ, &size) == 0 &&
	    size.ws_col > 0)
		return size.ws_col;
	configured = getenv("COLUMNS");
	if (!configured || !configured[0])
		return CLI_TERMINAL_DEFAULT_COLUMNS;
	errno = 0;
	columns = strtol(configured, &end, 10);
	if (errno != 0 || *end != '\0' || columns <= 0 ||
	    columns > INT_MAX)
		return CLI_TERMINAL_DEFAULT_COLUMNS;
	return (int)columns;
}

int cli_terminal_columns(const struct cli_context *ctx)
{
	return terminal_columns(ctx ? ctx->terminal : NULL);
}

static void terminal_clear_current(struct cli_terminal *terminal)
{
	if (!terminal || !terminal->is_terminal)
		return;
	fprintf(terminal->output, "\r\033[2K");
}

static void terminal_clear_frame(struct cli_terminal *terminal)
{
	int rows;

	if (!terminal || !terminal->is_terminal || !terminal->live_visible)
		return;
	rows = terminal->live_rows > 0 ? terminal->live_rows : 1;
	if (terminal->live_anchored) {
		terminal_clear_current(terminal);
		fprintf(terminal->output, "\033[1A");
	}
	for (int row = 0; row < rows; row++) {
		terminal_clear_current(terminal);
		if (row + 1 < rows)
			fprintf(terminal->output, "\033[1A");
	}
}

static void terminal_sanitize_line(char *text)
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

static int terminal_text_changed(const struct cli_terminal *terminal,
				 const char *text)
{
	const char *current;

	if (!terminal || !text)
		return 0;
	current = morph_buf_cstr(&terminal->live_text);
	return !current || strcmp(current, text) != 0;
}

int cli_terminal_init(struct cli_context *ctx, FILE *output, int output_fd)
{
	struct cli_terminal *terminal;
	int rc;

	if (!ctx)
		MORPH_RETURN(-EINVAL);
	terminal = calloc(1, sizeof(*terminal));
	if (!terminal)
		MORPH_RETURN(-ENOMEM);
	rc = morph_buf_init(&terminal->live_text, BUFSIZ);
	if (rc != 0) {
		free(terminal);
		MORPH_RETURN(rc);
	}
	terminal->output = output ? output : stdout;
	terminal->output_fd = output_fd;
	terminal->is_terminal = output_fd >= 0 && isatty(output_fd);
	terminal->transient = terminal->is_terminal && cli_color_enabled(ctx);
	terminal->columns = terminal_columns(terminal);
	ctx->terminal = terminal;
	return 0;
}

void cli_terminal_cleanup(struct cli_context *ctx)
{
	struct cli_terminal *terminal;

	if (!ctx || !ctx->terminal)
		return;
	terminal = ctx->terminal;
	cli_terminal_live_clear(ctx);
	cli_terminal_update_end(ctx);
	morph_buf_cleanup(&terminal->live_text);
	free(terminal);
	ctx->terminal = NULL;
}

static void terminal_live_set(struct cli_context *ctx, const char *text,
			      int independent, int reasoning)
{
	struct cli_terminal *terminal;
	char *clean;
	int changed;

	if (!ctx || !ctx->terminal || !text || !text[0])
		return;
	terminal = ctx->terminal;
	clean = utf8_dup_clamped(text, CLI_TERMINAL_STATUS_MAX);
	if (!clean)
		return;
	terminal_sanitize_line(clean);
	changed = terminal_text_changed(terminal, clean);
	if (changed) {
		morph_buf_reset(&terminal->live_text);
		if (morph_buf_puts(&terminal->live_text, clean) != 0) {
			free(clean);
			return;
		}
	}
	if (independent && (!terminal->live_independent || !terminal->live_active || changed))
		terminal->live_started_ms = terminal_now_ms();
	terminal->live_independent = independent;
	terminal->live_reasoning = reasoning;
	terminal->live_active = 1;
	/* Coalesce content separately from the lightweight animation frames. */
	int64_t next = terminal_now_ms() + CLI_TERMINAL_FRAME_MS;

	terminal->content_dirty = 1;
	if (terminal->next_content_ms > next)
		terminal->next_content_ms = next;
	if (terminal->next_frame_ms > next)
		terminal->next_frame_ms = next;
	if (!terminal->transient) {
		if (changed) {
			fprintf(terminal->output, "• %s\n", clean);
			fflush(terminal->output);
		}
		free(clean);
		return;
	}
	free(clean);
	cli_terminal_render_frame(ctx, !terminal->live_visible);
}

void cli_terminal_live_set(struct cli_context *ctx, const char *text)
{
	terminal_live_set(ctx, text, 0, 0);
}

void cli_terminal_reasoning_set(struct cli_context *ctx, const char *text)
{
	terminal_live_set(ctx, text, 0, 1);
}

void cli_terminal_activity_set(struct cli_context *ctx, const char *text)
{
	terminal_live_set(ctx, text, 1, 0);
}

int64_t cli_terminal_activity_elapsed_ms(const struct cli_context *ctx)
{
	const struct cli_terminal *terminal = ctx ? ctx->terminal : NULL;

	if (!terminal || !terminal->live_active || !terminal->live_independent)
		return -1;
	int64_t elapsed = terminal_now_ms() - terminal->live_started_ms;

	return elapsed > 0 ? elapsed : 0;
}

void cli_terminal_live_clear(struct cli_context *ctx)
{
	struct cli_terminal *terminal;

	if (!ctx || !ctx->terminal)
		return;
	terminal = ctx->terminal;
	if (!ctx->details_visible && terminal->transient && terminal->live_visible) {
		terminal_clear_frame(terminal);
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

void cli_terminal_queue_changed(struct cli_context *ctx)
{
	struct cli_terminal *terminal;

	if (!ctx || !ctx->terminal)
		return;
	terminal = ctx->terminal;
	terminal->next_frame_ms = 0;
	if (terminal->transient && !ctx->details_visible)
		cli_terminal_render_frame(ctx, 1);
}

/* cursor_rows is the cursor's distance below the start of the composer.
 * Keep Readline, queued input and tool output untouched on animation ticks. */
int cli_terminal_render_tick(struct cli_context *ctx, int cursor_rows)
{
	struct cli_terminal *terminal = ctx ? ctx->terminal : NULL;
	struct winsize size = {0};
	int64_t now = terminal_now_ms();
	int distance;

	if (!terminal || !terminal->transient || !terminal->live_visible ||
	    ctx->details_visible || terminal->work_state != CLI_WORK_RUNNING ||
	    terminal->columns < 20 || terminal_columns(terminal) != terminal->columns)
		return 0;
	if ((terminal->content_dirty || terminal->live_independent) &&
	    now >= terminal->next_content_ms)
		return 0;
	if (now < terminal->next_frame_ms)
		return 1;
	terminal->next_frame_ms = now + terminal_work_interval(terminal, now);
	distance = terminal->work_rows_below + cursor_rows + terminal->live_anchored;
	/* A long wrapped draft can push the status row out of the viewport. */
	if (ioctl(terminal->output_fd, TIOCGWINSZ, &size) == 0 &&
	    size.ws_row > 0 && distance >= size.ws_row)
		return 1;
	int updating = cli_terminal_update_active(ctx);

	cli_terminal_update_begin(ctx);
	fprintf(terminal->output, "\0337\r");
	if (distance > 0)
		fprintf(terminal->output, "\033[%dA", distance);
	terminal_work_row(terminal, terminal->columns);
	fprintf(terminal->output, "\0338");
	if (!updating)
		cli_terminal_update_end(ctx);
	return 1;
}

void cli_terminal_render_frame(struct cli_context *ctx, int force)
{
	struct cli_terminal *terminal;
	const char *text;
	char status_clipped[BUFSIZ];
	char queue_clipped[BUFSIZ];
	morph_buf_t tool_text = {0};
	morph_buf_t tool_body = {0};
	char *queue_items[CLI_TERMINAL_QUEUE_VISIBLE] = {0};
	size_t queue_total = 0;
	size_t queue_shown = 0;
	int64_t now;
	int columns;
	int budget;
	int tool_live = 0;
	int body_rows = 0;
	int status_visible;

	if (!ctx || !ctx->terminal || ctx->details_visible)
		return;
	terminal = ctx->terminal;
	if (!terminal->transient)
		return;
	status_visible = terminal->live_active;
	now = terminal_now_ms();
	columns = terminal_columns(terminal);
	if (columns != terminal->columns) {
		terminal->columns = columns;
		force = 1;
	}
	if (!force && terminal->live_visible &&
	    now < terminal->next_frame_ms)
		return;
	if (!force && !terminal->live_anchored && cli_terminal_render_tick(ctx, 0))
		return;
	if (ctx->input_job &&
	    cli_command_job_prompt_snapshot(ctx->input_job, queue_items,
					    CLI_TERMINAL_QUEUE_VISIBLE,
					    &queue_total) == 0) {
		queue_shown = queue_total < CLI_TERMINAL_QUEUE_VISIBLE ?
			queue_total : CLI_TERMINAL_QUEUE_VISIBLE;
	}
	/* Final output already owns its trailing separator. Match the completed
	 * work row even if persistence keeps the worker alive for another tick. */
	terminal->work_finalizing = ctx->final_rendered &&
		terminal->work_state == CLI_WORK_RUNNING && !status_visible && !queue_shown;
	if (!status_visible && queue_shown == 0 && !terminal->work_state) {
		if (terminal->live_visible) {
			terminal_clear_frame(terminal);
			fflush(terminal->output);
			terminal->live_visible = 0;
			terminal->live_anchored = 0;
			terminal->live_rows = 0;
		}
		return;
	}
	/* Below the layout minimum, keep one bounded row and keep accepting input. */
	if (columns < 20) {
		if (terminal->live_visible)
			terminal_clear_frame(terminal);
		else
			terminal_clear_current(terminal);
		(void)utf8_copy_ellipsized_display_width(status_clipped,
			sizeof(status_clipped), "Widen terminal", (size_t)(columns - 1), 0);
		fprintf(terminal->output, ANSI_DIM "%s" ANSI_RESET, status_clipped);
		fflush(terminal->output);
		for (size_t i = 0; i < queue_shown; i++)
			free(queue_items[i]);
		terminal->live_visible = 1;
		terminal->live_anchored = 0;
		terminal->live_rows = 1;
		terminal->next_frame_ms = now + CLI_TERMINAL_WORK_FRAME_MS;
		return;
	}
	text = morph_buf_cstr(&terminal->live_text);
	if (status_visible && morph_buf_init(&tool_text, 128) == 0) {
		if (terminal->live_independent) {
			char elapsed[32];
			int length = snprintf(elapsed, sizeof(elapsed), "%llds",
				(long long)(cli_terminal_activity_elapsed_ms(ctx) / 1000));
			int available = terminal->columns - 6 - length;

			(void)utf8_copy_ellipsized_display_width(status_clipped,
				sizeof(status_clipped), text,
				(size_t)(available > 0 ? available : 1), 0);
			(void)morph_buf_printf(&tool_text, "%s  %s", status_clipped, elapsed);
			text = morph_buf_cstr(&tool_text);
		} else if (!terminal->live_reasoning) {
			tool_live =
				cli_transcript_live_text(ctx, &tool_text, cli_color_enabled(ctx));
			if (tool_live)
				text = morph_buf_cstr(&tool_text);
		}
	}
	budget = terminal->columns - 4;
	if (budget < 1)
		budget = 1;
	(void)utf8_copy_sanitized_display_width(
		status_clipped, sizeof(status_clipped), text ? text : "",
		(size_t)budget);
	if (terminal->live_visible)
		terminal_clear_frame(terminal);
	else
		terminal_clear_current(terminal);
	if (status_visible) {
		/* Match the committed reasoning line from its first streaming frame. */
		if (tool_live || terminal->live_reasoning)
			fputc('\n', terminal->output);
		if (tool_live)
			fprintf(terminal->output, "%s", text);
		else if (terminal->live_reasoning)
			fprintf(terminal->output, ANSI_DIM "\033[3m%s" ANSI_RESET,
				status_clipped);
		else
			fprintf(terminal->output, ANSI_CYAN "%s" ANSI_RESET " %s%s" ANSI_RESET,
				CLI_FEED_RUNNING,
				terminal->live_independent ? ANSI_DIM : "", status_clipped);
		if (tool_live && morph_buf_init(&tool_body, 128) == 0) {
			body_rows =
				cli_transcript_live_body(ctx, &tool_body, cli_color_enabled(ctx));
			if (body_rows)
				fprintf(terminal->output, "\n%s", morph_buf_cstr(&tool_body));
		}
		if (queue_shown || terminal->work_state)
			fputc('\n', terminal->output);
	}
	if (terminal->work_state) {
		if (!terminal->work_finalizing)
			fputc('\n', terminal->output);
		terminal_work_row(terminal, terminal->columns);
		if (queue_shown)
			fputc('\n', terminal->output);
	}
	if (queue_shown) {
		const char *heading = terminal->columns >= 44 ?
			"Messages queued for the next step" : "Messages queued";

		if (status_visible || terminal->work_state)
			fputc('\n', terminal->output);
		fprintf(terminal->output, "  " ANSI_DIM "•" ANSI_RESET
			" \033[38;5;252m%s" ANSI_RESET, heading);
		if ((size_t)terminal->columns >= utf8_display_width(heading) + 25)
			fprintf(terminal->output, ANSI_DIM
				" (esc to interrupt)" ANSI_RESET);
	}
	for (size_t i = 0; i < queue_shown; i++) {
		terminal_sanitize_line(queue_items[i]);
		(void)utf8_copy_ellipsized_display_width(
			queue_clipped, sizeof(queue_clipped), queue_items[i],
			(size_t)(terminal->columns - 6), 0);
		fprintf(terminal->output, "\n    " ANSI_DIM "↳ %s" ANSI_RESET,
			queue_clipped);
		free(queue_items[i]);
	}
	if (queue_total > queue_shown)
		fprintf(terminal->output, "\n    " ANSI_DIM "+%zu more queued"
			ANSI_RESET, queue_total - queue_shown);
	morph_buf_cleanup(&tool_text);
	morph_buf_cleanup(&tool_body);
	fflush(terminal->output);
	terminal->live_visible = 1;
	terminal->live_anchored = 0;
	terminal->live_rows = status_visible + tool_live +
		(status_visible && terminal->live_reasoning) + body_rows +
		(terminal->work_state ? (terminal->work_finalizing ? 1 : 2) : 0) +
		(int)queue_shown + (queue_shown > 0) + (queue_total > queue_shown) +
		(queue_shown > 0 && (status_visible || terminal->work_state));
	terminal->work_rows_below = queue_shown ?
		2 + (int)queue_shown + (queue_total > queue_shown) : 0;
	terminal->content_dirty = 0;
	terminal->next_content_ms = now + CLI_TERMINAL_WORK_FRAME_MS;
	terminal->next_frame_ms = now + (terminal->work_state == CLI_WORK_RUNNING ?
		terminal_work_interval(terminal, now) : CLI_TERMINAL_WORK_FRAME_MS);
}

int cli_terminal_next_frame_ms(const struct cli_context *ctx)
{
	const struct cli_terminal *terminal;
	int64_t remaining;

	if (!ctx || !ctx->terminal)
		return -1;
	terminal = ctx->terminal;
	if (!terminal->transient ||
	    (!terminal->live_active && terminal->work_state != CLI_WORK_RUNNING))
		return -1;
	if (!terminal->live_visible)
		return 0;
	remaining = terminal->next_frame_ms - terminal_now_ms();
	if (remaining <= 0)
		return 0;
	return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

void cli_terminal_composer_suspend(struct cli_context *ctx)
{
	struct cli_terminal *terminal;

	if (!ctx || !ctx->terminal)
		return;
	terminal = ctx->terminal;
	if (!terminal->is_terminal)
		return;
	terminal_clear_current(terminal);
	while (ctx->input_status.padding_rows > 0) {
		fprintf(terminal->output, "\033[1A\r\033[2K");
		ctx->input_status.padding_rows--;
	}
	terminal_clear_frame(terminal);
	terminal->live_visible = 0;
	terminal->live_anchored = 0;
	terminal->live_rows = 0;
	fflush(terminal->output);
}

void cli_terminal_composer_resume(struct cli_context *ctx)
{
	struct cli_terminal *terminal;

	if (!ctx || !ctx->terminal)
		return;
	terminal = ctx->terminal;
	if (!terminal->transient)
		return;
	if (!terminal->live_visible)
		cli_terminal_render_frame(ctx, 1);
	if (terminal->live_visible && !terminal->live_anchored) {
		fputc('\n', terminal->output);
		fflush(terminal->output);
		terminal->live_anchored = 1;
	}
	if (ctx->input_status.enabled && !ctx->input_status.padding_rows) {
		/* Finalizing work uses the same reservation as its committed row. */
		int padding = terminal->live_visible && !terminal->work_finalizing ? 2 : 1;

		if (padding == 2)
			fprintf(terminal->output, ANSI_RESET "\033[2K\n");
		fprintf(terminal->output, CLI_INPUT_BACKGROUND "\033[2K\n" ANSI_RESET);
		ctx->input_status.padding_rows = padding;
	}
}

void cli_terminal_history_begin(struct cli_context *ctx)
{
	struct cli_terminal *terminal;

	if (!ctx || !ctx->terminal || ctx->details_visible)
		return;
	terminal = ctx->terminal;
	if (!terminal->transient || !terminal->live_visible)
		return;
	terminal_clear_frame(terminal);
	terminal->live_visible = 0;
	terminal->live_anchored = 0;
	terminal->live_rows = 0;
	fflush(terminal->output);
}

void cli_terminal_history_end(struct cli_context *ctx)
{
	if (!ctx || !ctx->terminal || !ctx->terminal->live_active)
		return;
	cli_terminal_render_frame(ctx, 1);
}

void cli_terminal_resize(struct cli_context *ctx)
{
	if (!ctx || !ctx->terminal)
		return;
	ctx->terminal->columns = terminal_columns(ctx->terminal);
	ctx->terminal->next_frame_ms = 0;
}

int cli_terminal_live_active(const struct cli_context *ctx)
{
	return ctx && ctx->terminal && ctx->terminal->live_active;
}
