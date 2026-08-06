#include "sapi/cli/internal.h"
#include "sapi/cli/terminal.h"

#include <sys/ioctl.h>

#define CLI_TERMINAL_DEFAULT_COLUMNS 80
#define CLI_TERMINAL_MIN_COLUMNS 20
#define CLI_TERMINAL_FRAME_MS 80
#define CLI_TERMINAL_STATUS_MAX 2000

struct cli_terminal {
	FILE *output;
	int output_fd;
	morph_buf_t live_text;
	int live_active;
	int live_visible;
	int live_anchored;
	int dirty;
	int is_terminal;
	int transient;
	int frame;
	int columns;
	int64_t next_frame_ms;
};

static int64_t terminal_now_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
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
	utf8_sanitize_inplace(clean);
	for (char *cur = clean; *cur; cur++) {
		unsigned char ch = (unsigned char)*cur;

		if (ch < 0x20u || ch == 0x7fu)
			*cur = ' ';
	}
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
	if (terminal->transient && terminal->live_visible) {
		if (terminal->live_anchored) {
			terminal_clear_current(terminal);
			fprintf(terminal->output, "\033[1A");
		}
		terminal_clear_current(terminal);
		fflush(terminal->output);
	}
	terminal->live_active = 0;
	terminal->live_visible = 0;
	terminal->live_anchored = 0;
	terminal->dirty = 0;
	terminal->frame = 0;
	terminal->next_frame_ms = 0;
	morph_buf_reset(&terminal->live_text);
}

void cli_terminal_render_frame(struct cli_context *ctx, int force)
{
	static const char *frames[] = {
		"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧",
		"⠇", "⠏",
	};
	struct cli_terminal *terminal;
	const char *text;
	char clipped[BUFSIZ];
	int64_t now;
	int columns;
	int budget;

	if (!ctx || !ctx->terminal)
		return;
	terminal = ctx->terminal;
	if (!terminal->transient || !terminal->live_active)
		return;
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
	text = morph_buf_cstr(&terminal->live_text);
	budget = terminal->columns - 4;
	if (budget < 1)
		budget = 1;
	(void)utf8_copy_sanitized_display_width(
		clipped, sizeof(clipped), text ? text : "", (size_t)budget);
	terminal_clear_current(terminal);
	fprintf(terminal->output, ANSI_CYAN "%s" ANSI_RESET " %s",
		frames[terminal->frame %
		       (int)(sizeof(frames) / sizeof(frames[0]))], clipped);
	fflush(terminal->output);
	terminal->frame++;
	terminal->dirty = 0;
	terminal->live_visible = 1;
	terminal->live_anchored = 0;
	terminal->next_frame_ms = now + CLI_TERMINAL_FRAME_MS;
}

int cli_terminal_next_frame_ms(const struct cli_context *ctx)
{
	const struct cli_terminal *terminal;
	int64_t remaining;

	if (!ctx || !ctx->terminal)
		return -1;
	terminal = ctx->terminal;
	if (!terminal->transient || !terminal->live_active)
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
	if (terminal->live_visible && terminal->live_anchored) {
		fprintf(terminal->output, "\033[1A");
		terminal_clear_current(terminal);
	}
	terminal->live_visible = 0;
	terminal->live_anchored = 0;
	fflush(terminal->output);
}

void cli_terminal_composer_resume(struct cli_context *ctx)
{
	struct cli_terminal *terminal;

	if (!ctx || !ctx->terminal)
		return;
	terminal = ctx->terminal;
	if (!terminal->transient || !terminal->live_active)
		return;
	if (!terminal->live_visible)
		cli_terminal_render_frame(ctx, 1);
	if (terminal->live_visible && !terminal->live_anchored) {
		fputc('\n', terminal->output);
		fflush(terminal->output);
		terminal->live_anchored = 1;
	}
}

void cli_terminal_history_begin(struct cli_context *ctx)
{
	struct cli_terminal *terminal;

	if (!ctx || !ctx->terminal)
		return;
	terminal = ctx->terminal;
	if (!terminal->transient || !terminal->live_visible)
		return;
	if (terminal->live_anchored) {
		terminal_clear_current(terminal);
		fprintf(terminal->output, "\033[1A");
	}
	terminal_clear_current(terminal);
	terminal->live_visible = 0;
	terminal->live_anchored = 0;
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
