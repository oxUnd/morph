#include "sapi/cli/internal.h"
#include "sapi/cli/terminal.h"

#include <sys/ioctl.h>

#define CLI_TERMINAL_DEFAULT_COLUMNS 80
#define CLI_TERMINAL_MIN_COLUMNS 20
#define CLI_TERMINAL_FRAME_MS 80
#define CLI_TERMINAL_WORK_FRAME_MS 120
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
	int live_visible;
	int live_anchored;
	int live_rows;
	int dirty;
	int is_terminal;
	int transient;
	int frame;
	int columns;
	int64_t next_frame_ms;
	enum cli_work_state work_state;
	int64_t work_started_ms;
	int64_t work_elapsed_ms;
	time_t work_finished_at;
};

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
	ctx->terminal->dirty = 1;
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
	terminal->dirty = 1;
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
	(void)morph_buf_puts(&text, terminal->work_state == CLI_WORK_RUNNING ? "(" :
		terminal->work_state == CLI_WORK_COMPLETED ? "Worked for " :
		terminal->work_state == CLI_WORK_STOPPED ? "Stopped after " : "Failed after ");
	if (seconds >= 60)
		(void)morph_buf_printf(&text, "%lldm ", (long long)(seconds / 60));
	(void)morph_buf_printf(&text, "%llds", (long long)(seconds % 60));
	if (terminal->work_state == CLI_WORK_RUNNING)
		(void)morph_buf_puts(&text, " · esc to interrupt)");
	else if (localtime_r(&terminal->work_finished_at, &finished)) {
		(void)strftime(clock, sizeof(clock), "%H:%M", &finished);
		(void)morph_buf_printf(&text, " · %s", clock);
	}
	int prefix = terminal->work_state == CLI_WORK_RUNNING ? 12 : 2;

	(void)utf8_copy_ellipsized_display_width(clipped, sizeof(clipped),
		morph_buf_cstr(&text), (size_t)(columns > prefix ? columns - prefix : 1), 0);
	if (terminal->work_state == CLI_WORK_RUNNING) {
		const char label[] = "Working";
		int position = (int)((elapsed / CLI_TERMINAL_WORK_FRAME_MS) %
			(int)(sizeof(label) + 3)) - 2;

		fprintf(terminal->output, "\033[38;5;246m● ");
		for (size_t i = 0; i < sizeof(label) - 1; i++) {
			int distance = abs((int)i - position);
			int shade = distance == 0 ? 255 : distance == 1 ? 250 :
				distance == 2 ? 244 : 240;

			fprintf(terminal->output, "\033[38;5;%dm%c", shade, label[i]);
		}
		fprintf(terminal->output, ANSI_RESET " ");
	}
	fprintf(terminal->output, ANSI_DIM "%s" ANSI_RESET, clipped);
	morph_buf_cleanup(&text);
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
		return size.ws_col < CLI_TERMINAL_MIN_COLUMNS ?
			CLI_TERMINAL_MIN_COLUMNS : size.ws_col;
	configured = getenv("COLUMNS");
	if (!configured || !configured[0])
		return CLI_TERMINAL_DEFAULT_COLUMNS;
	errno = 0;
	columns = strtol(configured, &end, 10);
	if (errno != 0 || *end != '\0' || columns <= 0 ||
	    columns > INT_MAX)
		return CLI_TERMINAL_DEFAULT_COLUMNS;
	return columns < CLI_TERMINAL_MIN_COLUMNS ?
		CLI_TERMINAL_MIN_COLUMNS : (int)columns;
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
	terminal->transient = terminal->is_terminal && cli_color_enabled();
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
	morph_buf_cleanup(&terminal->live_text);
	free(terminal);
	ctx->terminal = NULL;
}

void cli_terminal_live_set(struct cli_context *ctx, const char *text)
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
		terminal->dirty = 1;
	}
	terminal->live_active = 1;
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
	terminal->live_visible = 0;
	terminal->live_anchored = 0;
	terminal->live_rows = 0;
	terminal->dirty = 0;
	terminal->frame = 0;
	terminal->next_frame_ms = 0;
	morph_buf_reset(&terminal->live_text);
}

void cli_terminal_queue_changed(struct cli_context *ctx)
{
	struct cli_terminal *terminal;

	if (!ctx || !ctx->terminal)
		return;
	terminal = ctx->terminal;
	terminal->dirty = 1;
	terminal->next_frame_ms = 0;
	if (terminal->transient && !ctx->details_visible)
		cli_terminal_render_frame(ctx, 1);
}

void cli_terminal_render_frame(struct cli_context *ctx, int force)
{
	static const char *frames[] = {
		"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧",
		"⠇", "⠏",
	};
	struct cli_terminal *terminal;
	const char *text;
	char status_clipped[BUFSIZ];
	char queue_clipped[BUFSIZ];
	morph_buf_t tool_text = {0};
	char *queue_items[CLI_TERMINAL_QUEUE_VISIBLE] = {0};
	size_t queue_total = 0;
	size_t queue_shown = 0;
	int64_t now;
	int columns;
	int budget;
	int tool_live = 0;
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
		terminal->dirty = 1;
		force = 1;
	}
	if (!force && terminal->live_visible &&
	    now < terminal->next_frame_ms && !terminal->dirty)
		return;
	if (!force && terminal->live_visible &&
	    now < terminal->next_frame_ms)
		return;
	if (ctx->input_job &&
	    cli_command_job_prompt_snapshot(ctx->input_job, queue_items,
					    CLI_TERMINAL_QUEUE_VISIBLE,
					    &queue_total) == 0) {
		queue_shown = queue_total < CLI_TERMINAL_QUEUE_VISIBLE ?
			queue_total : CLI_TERMINAL_QUEUE_VISIBLE;
	}
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
	text = morph_buf_cstr(&terminal->live_text);
	if (status_visible && morph_buf_init(&tool_text, 128) == 0) {
		tool_live = cli_transcript_live_text(ctx, &tool_text, cli_color_enabled());
		if (tool_live)
			text = morph_buf_cstr(&tool_text);
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
		fprintf(terminal->output, "%s%s" ANSI_RESET " %s",
			tool_live ? ANSI_YELLOW : ANSI_CYAN,
			tool_live ? (terminal->frame % 2 ? "◉" : "◯") :
			frames[terminal->frame %
			       (int)(sizeof(frames) / sizeof(frames[0]))],
			tool_live ? text : status_clipped);
		if (queue_shown || terminal->work_state)
			fputc('\n', terminal->output);
	}
	if (terminal->work_state) {
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
	fflush(terminal->output);
	terminal->frame++;
	terminal->dirty = 0;
	terminal->live_visible = 1;
	terminal->live_anchored = 0;
	terminal->live_rows = status_visible + (terminal->work_state != CLI_WORK_NONE) +
		(int)queue_shown + (queue_shown > 0) + (queue_total > queue_shown) +
		(queue_shown > 0 && (status_visible || terminal->work_state));
	terminal->next_frame_ms = now +
		(terminal->live_active ? CLI_TERMINAL_FRAME_MS : CLI_TERMINAL_WORK_FRAME_MS);
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
		fprintf(terminal->output, ANSI_RESET "\033[2K\n"
			"\033[48;5;234m\033[2K\n" ANSI_RESET);
		ctx->input_status.padding_rows = 2;
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
	ctx->terminal->dirty = 1;
	ctx->terminal->next_frame_ms = 0;
}

int cli_terminal_live_active(const struct cli_context *ctx)
{
	return ctx && ctx->terminal && ctx->terminal->live_active;
}
