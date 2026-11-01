#include "sapi/cli/adapters/input/readline.h"
#include "sapi/cli/services/turn.h"
#ifdef HAVE_READLINE
#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>
#endif
#include "sapi/cli/services/command_job.h"
#include "credits.h"
#include "sapi/cli/adapters/input/reader.h"
#include "sapi/cli/adapters/input/keymap.h"
#include "sapi/cli/adapters/input/clipboard.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/services/recap.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/transcript.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/adapters/input/callbacks.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/models/composer.h"
#include <sys/ioctl.h>

#define CLI_READLINE_DETAILS_INPUT_BURST_MAX 1024

#ifdef HAVE_READLINE

/* Readline still owns editing and wrapping. Apply color to image labels after
 * redisplay, restoring the cursor and attributes before accepting more input. */
static void cli_readline_position(struct cli_application *ctx, int offset, int columns, int *row,
				  int *column)
{
	(void)ctx;
	const char *cursor = rl_line_buffer;
	const char *end = cursor + offset;

	*row = 0;
	*column = 2;
	while (cursor < end) {
		utf8_int32_t cp;
		int width;

		cursor = utf8codepoint(cursor, &cp);
		if (cp == '\n') {
			(*row)++;
			*column = 0;
			continue;
		}
		width = cp == '\t' ? 8 - *column % 8 : utf8_codepoint_width((unsigned)cp);
		if (width < 0)
			width = 2;
		if (*column + width > columns) {
			(*row)++;
			*column = 0;
		}
		*column += width;
		if (*column >= columns) {
			(*row)++;
			*column = 0;
		}
	}
}

static void cli_readline_color_images(struct cli_application *ctx)
{
	struct winsize size;
	int cursor_row;
	int cursor_column;
	int last_row;
	int last_column;
	const char *found = rl_line_buffer;
	FILE *output = rl_outstream ? rl_outstream : stdout;

	if (!RL_ISSTATE(RL_STATE_CALLBACK) || !cli_output_color_enabled(ctx) || !rl_line_buffer ||
	    ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 || size.ws_col < 2 || size.ws_row < 2)
		return;
	cli_readline_position(ctx, rl_point, size.ws_col, &cursor_row, &cursor_column);
	cli_readline_position(ctx, rl_end, size.ws_col, &last_row, &last_column);
	if (last_row >= size.ws_row - 1)
		return;
	while ((found = strchr(found, '['))) {
		int start;
		int end;
		int row;
		int column;

		if (!cli_composer_model_image_span(&ctx->composer, rl_line_buffer,
						   (int)(found - rl_line_buffer), 0, &start,
						   &end)) {
			found++;
			continue;
		}
		cli_readline_position(ctx, start, size.ws_col, &row, &column);
		fprintf(output, "\0337\r");
		if (row < cursor_row)
			fprintf(output, "\033[%dA", cursor_row - row);
		else if (row > cursor_row)
			fprintf(output, "\033[%dB", row - cursor_row);
		fprintf(output, "\033[%dG\033[48;5;24m\033[38;5;153m%.*s\033[0m\0338", column + 1,
			end - start, rl_line_buffer + start);
		found = rl_line_buffer + end;
	}
	fflush(output);
}

void cli_readline_clear_footer(struct cli_application *ctx)
{
	if (!ctx || !ctx->input_status.distance)
		return;
	fprintf(stdout, "\0337\033[%dB\r\033[2K", ctx->input_status.distance);
	for (int i = 0; i < ctx->input_status.footer_rows; i++)
		fprintf(stdout, "\033[1A\r\033[2K");
	fprintf(stdout, "\0338");
	ctx->input_status.distance = 0;
	ctx->input_status.footer_rows = 0;
}

static void cli_readline_footer_field(struct cli_application *ctx, morph_buf_t *text,
				      const char *value, size_t width, const char *color,
				      int keep_tail)
{
	char clipped[BUFSIZ];
	char *safe = utf8_terminal_sanitize_dup(value, strlen(value),
						UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);

	if (!safe)
		return;
	(void)utf8_copy_ellipsized_display_width(clipped, sizeof(clipped), safe, width, keep_tail);
	(void)morph_buf_printf(text, "%s%s%s", cli_output_color_enabled(ctx) ? color : "", clipped,
			       cli_output_color_enabled(ctx) ? ANSI_RESET : "");
	free(safe);
}

static void cli_readline_footer_tokens(morph_buf_t *text, int tokens)
{
	if (tokens >= 1000000)
		(void)morph_buf_printf(text, "%.3gM", (double)tokens / 1000000.0);
	else if (tokens >= 1000)
		(void)morph_buf_printf(text, "%.3gk", (double)tokens / 1000.0);
	else
		(void)morph_buf_printf(text, "%d", tokens);
}

static void cli_readline_footer(struct cli_application *ctx)
{
	struct winsize size = {0};
	struct timespec now;
	morph_buf_t text;
	morph_buf_t usage;
	struct session session = {0};
	size_t remaining;
	size_t model_width;
	const char *workdir;
	const char *home = getenv("HOME");
	int row, column, last_row, last_column;
	int footer_rows;

	if (!ctx || !ctx->runtime || ctx->input_status.suspended ||
	    !RL_ISSTATE(RL_STATE_CALLBACK) || !rl_line_buffer || !isatty(STDOUT_FILENO) ||
	    ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 || size.ws_col < 20 || size.ws_row < 3)
		return;
	cli_readline_position(ctx, rl_point, size.ws_col, &row, &column);
	cli_readline_position(ctx, rl_end, size.ws_col, &last_row, &last_column);
	ctx->input_status.composer_rows = last_row + 1;
	if (last_row >= size.ws_row - 2)
		return;
	if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec != ctx->input_status.updated) {
		struct credit_summary credits = {0};

		(void)runtime_session_model_context_stats(ctx->runtime, NULL,
							  &ctx->input_status.tokens, NULL, NULL,
							  &ctx->input_status.limit);
		if (ctx->input_status.limit <= 0)
			ctx->input_status.limit =
				runtime_config_get(ctx->runtime)->models.text.context_limit;
		if (runtime_credit_summary_current_get(ctx->runtime, &credits) == 0)
			ctx->input_status.credits = credits.credits;
		(void)runtime_task_count_active(ctx->runtime, &ctx->input_status.tasks);
		ctx->input_status.updated = now.tv_sec;
	}
	if (morph_buf_init(&text, 128) != 0)
		return;
	if (morph_buf_init(&usage, 64) != 0) {
		morph_buf_cleanup(&text);
		return;
	}

	{
		int total = runtime_mcp_count(ctx->runtime);
		int ready = 0, pending = 0, failed = 0;

		for (int i = 0; i < total; i++) {
			struct runtime_mcp_status status;

			if (runtime_mcp_info(ctx->runtime, i, &status) != 0)
				continue;
			ready += status.state == RUNTIME_MCP_READY;
			failed += status.state == RUNTIME_MCP_FAILED;
			pending += status.state == RUNTIME_MCP_CONNECTING ||
				   status.state == RUNTIME_MCP_DISCOVERING ||
				   status.state == RUNTIME_MCP_REGISTERING;
		}
		if (total > 0)
			(void)morph_buf_printf(&usage, "MCP %d/%d%s%s · ", ready, total,
					       pending ? " ..." : "", failed ? " !" : "");
	}
	(void)morph_buf_puts(&usage, size.ws_col >= 100 ? "context " : "ctx ");
	cli_readline_footer_tokens(&usage, ctx->input_status.tokens);
	(void)morph_buf_puts(&usage, " / ");
	cli_readline_footer_tokens(&usage, ctx->input_status.limit);
	(void)morph_buf_printf(&usage, size.ws_col >= 100 ? "  ·  %lld credits" : " · %lld cr",
			       (long long)ctx->input_status.credits);
	if (ctx->input_status.session_titles_pending > 0)
		(void)morph_buf_puts(&usage, " · Naming session...");
	if (ctx->recap.pending)
		(void)morph_buf_puts(&usage, " · Generating recap...");
	(void)morph_buf_puts(&text, "  ");
	cli_readline_footer_field(ctx, &text, "morph", 5, ANSI_BOLD, 0);
	(void)morph_buf_puts(&text, " · ");
	remaining = (size_t)size.ws_col - 2 - utf8_display_width_ansi(morph_buf_cstr(&text));
	if (remaining > utf8_display_width(morph_buf_cstr(&usage)) + 10) {
		remaining -= utf8_display_width(morph_buf_cstr(&usage)) + 6;
		model_width = remaining / 3;
		if (model_width > 24)
			model_width = 24;
		(void)runtime_session_current(ctx->runtime, &session);
		cli_readline_footer_field(
			ctx, &text,
			session.model[0] ? session.model
					 : runtime_config_get(ctx->runtime)->models.text.model,
			model_width, ANSI_DIM, 0);
		(void)morph_buf_puts(&text, " · ");
		workdir = runtime_workdir_get(ctx->runtime);
		if (!workdir || !workdir[0])
			workdir = ctx->workdir;
		if (home && home[0] && strncmp(workdir, home, strlen(home)) == 0 &&
		    (workdir[strlen(home)] == '/' || workdir[strlen(home)] == '\0')) {
			(void)morph_buf_puts(&text,
					     cli_output_color_enabled(ctx) ? ANSI_DIM "~" : "~");
			workdir += strlen(home);
			remaining--;
		}
		cli_readline_footer_field(ctx, &text, workdir, remaining - model_width, ANSI_DIM,
					  1);
		(void)morph_buf_puts(&text, " · ");
	}
	cli_readline_footer_field(ctx, &text, morph_buf_cstr(&usage),
				  (size_t)size.ws_col - 2 -
					  utf8_display_width_ansi(morph_buf_cstr(&text)),
				  ANSI_DIM, 0);
	footer_rows = 1;
	if (ctx->input_status.tasks.running || ctx->input_status.tasks.pending) {
		morph_buf_t tasks;

		if (morph_buf_init(&tasks, 64) == 0) {
			(void)morph_buf_printf(&tasks, "Tasks %lld running · %lld pending",
					       (long long)ctx->input_status.tasks.running,
					       (long long)ctx->input_status.tasks.pending);
			(void)morph_buf_puts(&text, "\r\n\033[2K  ");
			cli_readline_footer_field(
				ctx, &text, morph_buf_cstr(&tasks), (size_t)size.ws_col - 4,
				ctx->input_status.tasks.running ? ANSI_YELLOW : ANSI_DIM, 0);
			morph_buf_cleanup(&tasks);
			footer_rows++;
		}
	}
	/* A background poll must not repaint an unchanged footer or touch the draft. */
	if (ctx->input_status.distance == last_row - row + footer_rows + 1 &&
	    ctx->input_status.footer_rows == footer_rows && ctx->footer_text &&
	    strcmp(ctx->footer_text, morph_buf_cstr(&text)) == 0) {
		morph_buf_cleanup(&usage);
		morph_buf_cleanup(&text);
		return;
	}
	free(ctx->footer_text);
	ctx->footer_text = strdup(morph_buf_cstr(&text));
	cli_readline_clear_footer(ctx);
	/* Reserve a padded input row and footer even at screen bottom. */
	if (last_row > row)
		fprintf(stdout, "\033[%dB", last_row - row);
	fprintf(stdout,
		"\033[%dG%s\033[K\r\n\033[2K%s\r\n\033[2K%s"
		"\033[%dA\033[%dG",
		last_column + 1, cli_output_color_enabled(ctx) ? CLI_INPUT_BACKGROUND : "",
		cli_output_color_enabled(ctx) ? ANSI_RESET : "", morph_buf_cstr(&text),
		last_row - row + footer_rows + 1, column + 1);
	ctx->input_status.distance = last_row - row + footer_rows + 1;
	ctx->input_status.footer_rows = footer_rows;
	morph_buf_cleanup(&usage);
	morph_buf_cleanup(&text);
	fflush(stdout);
}

static void cli_readline_clear_hint(struct cli_application *ctx)
{
	if (!ctx->details_hint_visible)
		return;
	fprintf(stdout, "\0337\033[%dG\033[K\0338", ctx->details_hint_column);
	ctx->details_hint_visible = 0;
}

void cli_readline_redisplay(struct cli_application *ctx)
{
	struct winsize size = {0};

	if (ctx && ctx->details_visible)
		return;
	cli_readline_clear_footer(ctx);
	cli_readline_clear_hint(ctx);
	if (cli_output_color_enabled(ctx))
		fputs(ctx, CLI_INPUT_BACKGROUND, stdout);
	rl_redisplay();
	if (cli_output_color_enabled(ctx))
		fputs(ctx, ANSI_RESET, stdout);
	cli_readline_color_images(ctx);
	cli_readline_footer(ctx);
	if (!ctx->input_status.suspended && RL_ISSTATE(RL_STATE_CALLBACK) && rl_end == 0 &&
	    isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 &&
	    size.ws_col > 24) {
		ctx->details_hint_column = size.ws_col - 17;
		fprintf(stdout,
			"\0337\033[%dG" ANSI_DIM CLI_INPUT_BACKGROUND "ctrl+o details" ANSI_RESET
			"\0338",
			ctx->details_hint_column);
		ctx->details_hint_visible = 1;
		fflush(stdout);
	}
}

void cli_readline_line_ready(struct cli_application *ctx, char *input)
{

	if (!ctx) {
		free(input);
		return;
	}
	if (!input) {
		ctx->running = 0;
		if (ctx->input_job && ctx->input_job->active) {
			runtime_cancel_turn(ctx->runtime);
		}
		return;
	}
	/* The event loop installs the next composer after committing this input.
	 * Stop Readline from painting an automatic empty prompt in between. */
	rl_callback_handler_remove();
	free(ctx->readline_ready_input);
	ctx->readline_ready_input = input;
}

/* Readline owns the whole editable area, including wrapped and pasted lines.
 * Erase it through Readline before moving up to the live status row. */
char *cli_readline_suspend(struct cli_application *ctx, int *point)
{
	char *draft = strdup(rl_line_buffer ? rl_line_buffer : "");
	int row;
	int column;

	*point = rl_point;
	if (!draft)
		return NULL;
	cli_readline_position(ctx, rl_end, cli_feed_columns(ctx), &row, &column);
	ctx->input_status.composer_rows = row + 1;
	cli_transcript_view_suspend(ctx);
	cli_readline_clear_footer(ctx);
	ctx->input_status.suspended = 1;
	rl_replace_line("", 0);
	rl_point = 0;
	cli_readline_redisplay(ctx);
	cli_readline_clear_hint(ctx);
	cli_feed_composer_suspend(ctx);
	return draft;
}

void cli_readline_resume(struct cli_application *ctx, char *draft, int point)
{
	ctx->input_status.suspended = 0;
	cli_feed_composer_resume(ctx);
	/* Resize and callback installation may already have painted a prompt.
	 * rl_on_new_line assumes column zero on an empty row. */
	if (isatty(STDOUT_FILENO))
		fprintf(stdout, "\r\033[2K");
	rl_replace_line(draft, 0);
	rl_point = point;
	rl_on_new_line();
	rl_forced_update_display();
	free(draft);
	cli_transcript_view_resume(ctx);
}

/* Finish while the composer is suspended, so final output and the idle
 * composer share one synchronized update rather than a temporary live frame. */
void cli_readline_finish_job(struct cli_application *ctx)
{
	struct cli_command_job *job = ctx->input_job;
	int turn_rc = cli_command_job_finish(job);
	char *display = NULL;

	(void)cli_event_queue_drain(ctx);
	cli_turn_finish(ctx, turn_rc);
	ctx->input_status.updated = 0;
	char *pending = cli_command_job_take_prompt(job, &display);

	if (pending) {
		cli_presentation_user_input(ctx, display ? display : pending);
		cli_turn_begin(ctx);
		turn_rc = cli_command_job_start(job, ctx, pending);
		free(pending);
		if (turn_rc != 0)
			cli_turn_finish(ctx, turn_rc);
	}
	free(display);
}

void cli_readline_drain_ui(struct cli_application *ctx)
{
	int point;
	int viewing = ctx->details_visible;
	char *draft;

	if (viewing) {
		point = rl_point;
		draft = strdup(rl_line_buffer ? rl_line_buffer : "");
	} else {
		cli_feed_update_begin(ctx);
		draft = cli_readline_suspend(ctx, &point);
	}

	if (!draft) {
		cli_feed_update_end(ctx);
		return;
	}
	(void)cli_event_queue_drain_batch(ctx);
	if (!ctx->details_open && cli_command_job_done(ctx->input_job))
		cli_readline_finish_job(ctx);
	if (!ctx->details_visible)
		cli_feed_composer_resume(ctx);
	/* Installing a callback paints its prompt immediately. Restore with an
	 * empty prompt so the draft redraw is the sole prompt renderer. */
	if (!RL_ISSTATE(RL_STATE_CALLBACK)) {
		rl_callback_handler_install("", cli_callback_line(ctx, cli_readline_line_ready));
		cli_readline_configure(ctx);
		rl_set_prompt(cli_readline_input_prompt(ctx));
	}
	if (ctx->details_visible) {
		rl_replace_line(draft, 0);
		rl_point = point;
		free(draft);
		cli_transcript_view_render(ctx, 0);
	} else {
		cli_readline_resume(ctx, draft, point);
	}
	cli_feed_update_end(ctx);
}

void cli_readline_render_frame(struct cli_application *ctx, int resized)
{
	int point;
	char *draft;

	if (resized)
		(void)cli_markdown_stream_refresh(ctx);
	if (ctx->details_visible) {
		if (resized)
			cli_feed_resize(ctx);
		cli_transcript_view_render(ctx, 0);
		return;
	}
	if (!resized && cli_feed_next_frame_ms(ctx) < 0) {
		cli_readline_footer(ctx);
		return;
	}
	if (!resized && rl_line_buffer) {
		int row, column;

		cli_readline_position(ctx, rl_point, cli_feed_columns(ctx), &row, &column);
		if (cli_feed_render_tick(ctx, row + ctx->input_status.padding_rows)) {
			struct timespec now;

			if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
			    now.tv_sec != ctx->input_status.updated)
				cli_readline_footer(ctx);
			return;
		}
	}
	cli_feed_update_begin(ctx);
	draft = cli_readline_suspend(ctx, &point);

	if (!draft) {
		cli_feed_update_end(ctx);
		return;
	}
	if (resized) {
		struct winsize size = {0};

		/* rl_resize_terminal also repaints the suspended prompt. Update
		 * geometry without drawing; resume owns the sole composer redraw. */
		if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0)
			rl_set_screen_size(size.ws_row, size.ws_col);
		cli_feed_resize(ctx);
	}
	cli_feed_render_frame(ctx, resized);
	cli_readline_resume(ctx, draft, point);
	cli_feed_update_end(ctx);
}

static int cli_readline_convert_images(struct cli_application *ctx)
{
	char *converted = NULL;
	const char *input = rl_line_buffer;
	int rc;

	if (!RL_ISSTATE(RL_STATE_CALLBACK))
		return 0;
	if (strncmp(input, "/image", 6) == 0 && isspace((unsigned char)input[6]))
		input += 7;
	else if (input[0] == '/') {
		char *name = strndup(input, strcspn(input, " \t\r\n"));
		int command;

		if (!name)
			return -ENOMEM;
		command = cli_command_find(name) != NULL;
		free(name);
		if (command)
			return 0;
	}

	rc = cli_composer_model_convert_paths(&ctx->composer, input, &converted);
	if (rc > 0) {
		rl_begin_undo_group();
		rl_delete_text(0, rl_end);
		rl_point = 0;
		(void)rl_insert_text(converted);
		rl_end_undo_group();
		cli_readline_redisplay(ctx);
	}
	free(converted);
	return rc;
}

static int cli_readline_compose_image(struct cli_application *ctx, const char *path)
{
	const char *label;
	int rc;

	(void)ctx;
	rc = cli_composer_model_add_image(&ctx->composer, path, &label);
	if (rc != 0)
		return rc;
	rl_begin_undo_group();
	(void)rl_insert_text(label);
	rl_end_undo_group();
	cli_readline_redisplay(ctx);
	return 0;
}

static int cli_readline_image_edit(struct cli_application *ctx, int count, int key, int backward,
				   int erase)
{
	for (int i = 0; i < count; i++) {
		int start;
		int end;

		if (RL_ISSTATE(RL_STATE_CALLBACK) &&
		    cli_composer_model_image_span(&ctx->composer, rl_line_buffer, rl_point,
						  backward, &start, &end)) {
			if (erase)
				rl_delete_text(start, end);
			rl_point = erase || backward ? start : end;
		} else if (erase) {
			if (backward)
				rl_rubout(1, key);
			else
				rl_delete(1, key);
		} else if (backward) {
			rl_backward_char(1, key);
		} else {
			rl_forward_char(1, key);
		}
	}
	return 0;
}

static int cli_readline_backspace(struct cli_application *ctx, int count, int key)
{
	return cli_readline_image_edit(ctx, count, key, 1, 1);
}

static int cli_readline_delete_word(struct cli_application *ctx, int count, int key)
{
	int start;
	int end;

	if (cli_composer_model_image_span(&ctx->composer, rl_line_buffer, rl_point, 1, &start,
					  &end))
		return cli_readline_backspace(ctx, count, key);
	return rl_unix_word_rubout(count, key);
}

static int cli_readline_delete(struct cli_application *ctx, int count, int key)
{
	return cli_readline_image_edit(ctx, count, key, 0, 1);
}

static int cli_readline_backward(struct cli_application *ctx, int count, int key)
{
	return cli_readline_image_edit(ctx, count, key, 1, 0);
}

static int cli_readline_forward(struct cli_application *ctx, int count, int key)
{
	return cli_readline_image_edit(ctx, count, key, 0, 0);
}

#ifdef HAVE_RL_BRACKETED_PASTE_BEGIN
static int cli_readline_paste(struct cli_application *ctx, int count, int key)
{
	int rc = rl_bracketed_paste_begin(count, key);

	if (rc == 0)
		(void)cli_readline_convert_images(ctx);
	return rc;
}
#endif

static int cli_readline_accept(struct cli_application *ctx, int count, int key)
{
	int point;
	char *draft;

	/* Empty Enter should not commit an orphan prompt into the history.
	 * Blocking approval/question prompts still accept an empty answer. */
	if (RL_ISSTATE(RL_STATE_CALLBACK) && strspn(rl_line_buffer, " \t\r\n") == (size_t)rl_end)
		return 0;
	if (cli_readline_convert_images(ctx) != 0)
		return 0;
	draft = cli_readline_suspend(ctx, &point);

	if (!draft)
		return 1;
	if (RL_ISSTATE(RL_STATE_CALLBACK)) {
		/* Commit scrollback after Readline finishes its final redisplay. */
		rl_replace_line(draft, 0);
		rl_point = point;
		rl_done = 1;
		free(draft);
		return 0;
	}
	/* Commit the submitted input where the live row was, so accepting a
	 * line never leaves an old spinner behind in terminal history. */
	rl_replace_line(draft, 0);
	rl_point = point;
	rl_on_new_line();
	rl_forced_update_display();
	free(draft);
	return rl_newline(count, key);
}

static int cli_readline_insert_newline(struct cli_application *ctx, int count, int key)
{
	(void)key;
	for (int i = 0; i < count; i++) {
		if (rl_insert_text("\n") != 0)
			return 1;
	}
	cli_readline_redisplay(ctx);
	return 0;
}

static int cli_readline_paste_image(struct cli_application *ctx, int count, int key)
{
	char *path = NULL;
	int rc;

	(void)count;
	(void)key;
	if (!ctx || !RL_ISSTATE(RL_STATE_CALLBACK))
		return 1;
	rc = cli_clipboard_save_image(ctx, &path);
	if (rc == 0)
		rc = cli_readline_compose_image(ctx, path);
	if (rc != 0) {
		int point;
		char *draft = cli_readline_suspend(ctx, &point);

		if (path)
			(void)unlink(path);
		if (draft) {
			CMD_ERROR(ctx, "clipboard does not contain a supported image");
			cli_readline_resume(ctx, draft, point);
		}
		putchar(ctx, '\a');
	}
	free(path);
	return rc == 0 ? 0 : 1;
}

static int cli_readline_toggle_details(struct cli_application *ctx, int count, int key)
{
	(void)count;
	(void)key;
	if (!ctx || !RL_ISSTATE(RL_STATE_CALLBACK))
		return 0;
	if (!ctx->details_open) {
		int point;
		char *draft = cli_readline_suspend(ctx, &point);

		if (!draft)
			return 0;
		rl_replace_line(draft, 0);
		rl_point = point;
		free(draft);
	}
	cli_transcript_toggle(ctx);
	return 0;
}

static void cli_readline_close_details(struct cli_application *ctx)
{
	int point = rl_point;
	char *draft = strdup(rl_line_buffer ? rl_line_buffer : "");

	cli_transcript_toggle(ctx);
	if (draft)
		cli_readline_resume(ctx, draft, point);
}

static int cli_readline_details_escape(struct cli_application *ctx, int page, int *scroll)
{
	char sequence[16] = {0};
	size_t count = 0;
	struct pollfd fd = {.fd = STDIN_FILENO, .events = POLLIN};
	unsigned button;
	unsigned column;
	unsigned row;
	char action;

	while (count < sizeof(sequence) - 1 && poll(&fd, 1, 30) > 0) {
		if (read(STDIN_FILENO, sequence + count, 1) != 1)
			break;
		count++;
		if ((count > 1 && sequence[count - 1] >= 'A' && sequence[count - 1] <= 'Z') ||
		    sequence[count - 1] == '~' || sequence[count - 1] == 'm')
			break;
	}
	if (!count)
		return 1;
	if (strcmp(sequence, "[A") == 0)
		*scroll = -1;
	else if (strcmp(sequence, "[B") == 0)
		*scroll = 1;
	else if (strcmp(sequence, "[5~") == 0)
		*scroll = -page;
	else if (strcmp(sequence, "[6~") == 0)
		*scroll = page;
	else if (strcmp(sequence, "[H") == 0 || strcmp(sequence, "[1~") == 0)
		*scroll = INT_MIN;
	else if (strcmp(sequence, "[F") == 0 || strcmp(sequence, "[4~") == 0)
		*scroll = INT_MAX;
	else if (strcmp(sequence, "[I") == 0)
		cli_recap_focus(ctx, 1, cli_recap_now());
	else if (strcmp(sequence, "[O") == 0)
		cli_recap_focus(ctx, 0, cli_recap_now());
	else if (sscanf(sequence, "[<%u;%u;%u%c", &button, &column, &row, &action) == 4 &&
		 action == 'M' && (button & 64U))
		*scroll = (button & 1U) ? 3 : -3;
	return 0;
}

void cli_readline_details_input(struct cli_application *ctx)
{
	struct winsize size = {0};
	struct pollfd fd = {.fd = STDIN_FILENO, .events = POLLIN};
	int pending_scroll = 0;
	int page = 20;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row > 4)
		page = size.ws_row - 4;
	for (int i = 0; i < CLI_READLINE_DETAILS_INPUT_BURST_MAX; i++) {
		unsigned char key;
		int scroll = 0;

		if (read(STDIN_FILENO, &key, 1) != 1) {
			cli_readline_close_details(ctx);
			return;
		}
		if (key == 0x1b) {
			if (cli_readline_details_escape(ctx, page, &scroll)) {
				cli_readline_close_details(ctx);
				return;
			}
		} else if (key == 0x0f) {
			cli_readline_close_details(ctx);
			return;
		}
		if (scroll == INT_MIN || scroll == INT_MAX) {
			if (pending_scroll)
				cli_transcript_view_render(ctx, pending_scroll);
			cli_transcript_view_render(ctx, scroll);
			pending_scroll = 0;
		} else if (scroll) {
			if ((scroll > 0 && pending_scroll <= INT_MAX - scroll) ||
			    (scroll < 0 && pending_scroll >= INT_MIN - scroll))
				pending_scroll += scroll;
		}
		fd.revents = 0;
		if (poll(&fd, 1, 0) <= 0)
			break;
	}
	if (pending_scroll)
		cli_transcript_view_render(ctx, pending_scroll);
}

#ifdef HAVE_RL_BIND_KEYSEQ
static int cli_readline_focus_in(struct cli_application *ctx, int count, int key)
{
	(void)count;
	(void)key;
	cli_recap_focus(ctx, 1, cli_recap_now());
	return 0;
}

static int cli_readline_focus_out(struct cli_application *ctx, int count, int key)
{
	(void)count;
	(void)key;
	cli_recap_focus(ctx, 0, cli_recap_now());
	return 0;
}
#endif

void cli_readline_configure(struct cli_application *ctx)
{
	(void)rl_variable_bind("enable-bracketed-paste", "on");
	(void)rl_variable_bind("horizontal-scroll-mode", "off");
	(void)rl_bind_key('\r', cli_callback_key(ctx, cli_readline_accept));
	(void)rl_bind_key('\n', cli_callback_key(ctx, cli_readline_insert_newline));
	(void)rl_bind_key(0x16, cli_callback_key(ctx, cli_readline_paste_image));
	(void)rl_bind_key(0x0f, cli_callback_key(ctx, cli_readline_toggle_details));
	(void)rl_bind_key(0x7f, cli_callback_key(ctx, cli_readline_backspace));
	(void)rl_bind_key(0x08, cli_callback_key(ctx, cli_readline_backspace));
	(void)rl_bind_key(0x04, cli_callback_key(ctx, cli_readline_delete));
	(void)rl_bind_key(0x17, cli_callback_key(ctx, cli_readline_delete_word));
	(void)rl_bind_key(0x02, cli_callback_key(ctx, cli_readline_backward));
	(void)rl_bind_key(0x06, cli_callback_key(ctx, cli_readline_forward));
#ifdef HAVE_RL_BIND_KEYSEQ
	(void)rl_bind_keyseq("\033[I", cli_callback_key(ctx, cli_readline_focus_in));
	(void)rl_bind_keyseq("\033[O", cli_callback_key(ctx, cli_readline_focus_out));
	if (isatty(STDOUT_FILENO))
		fprintf(stdout, "\033[?1004h");
	(void)rl_bind_keyseq("\033[D", cli_callback_key(ctx, cli_readline_backward));
	(void)rl_bind_keyseq("\033[C", cli_callback_key(ctx, cli_readline_forward));
	(void)rl_bind_keyseq("\033[3~", cli_callback_key(ctx, cli_readline_delete));
#ifdef HAVE_RL_BRACKETED_PASTE_BEGIN
	(void)rl_bind_keyseq("\033[200~", cli_callback_key(ctx, cli_readline_paste));
#endif
	(void)rl_bind_keyseq("\\e\\C-M", cli_callback_key(ctx, cli_readline_insert_newline));
	(void)rl_bind_keyseq("\033[13;2u", cli_callback_key(ctx, cli_readline_insert_newline));
	(void)rl_bind_keyseq("\033[27;2;13~", cli_callback_key(ctx, cli_readline_insert_newline));
	/* Ctrl+Command+V on macOS (kitty keyboard protocol):
	 * modifier = 1 + (ctrl=4 | super=8) = 13, keycode for V base key = 118,
	 * with 86 as an upper-case fallback some terminals emit. */
	(void)rl_bind_keyseq("\033[118;13u", cli_callback_key(ctx, cli_readline_paste_image));
	(void)rl_bind_keyseq("\033[86;13u", cli_callback_key(ctx, cli_readline_paste_image));
#endif
}

void cli_readline_editor_open(struct cli_application *ctx, char *draft, int point)
{
	cli_feed_composer_resume(ctx);
	rl_callback_handler_install(draft ? "" : cli_readline_input_prompt(ctx),
				    cli_callback_line(ctx, cli_readline_line_ready));
	cli_readline_configure(ctx);
	if (draft) {
		rl_set_prompt(cli_readline_input_prompt(ctx));
		cli_readline_resume(ctx, draft, point);
	}
}

void cli_readline_editor_close(void)
{
	rl_callback_handler_remove();
}

void cli_readline_editor_replace(struct cli_application *ctx, const char *text, int point)
{
	rl_replace_line(text, 0);
	rl_point = point < 0 ? rl_end : point;
	cli_readline_redisplay(ctx);
}

void cli_readline_editor_read(struct cli_application *ctx)
{
	int previous_point = rl_point;
	int image_start;
	int image_end;

	rl_callback_read_char();
	if (!ctx->readline_ready_input &&
	    cli_composer_model_image_span(&ctx->composer, rl_line_buffer, rl_point, 0, &image_start,
					  &image_end) &&
	    rl_point > image_start) {
		rl_point = rl_point < previous_point ? image_start : image_end;
		cli_readline_redisplay(ctx);
	}
}

void cli_readline_editor_remember(const char *input)
{
	add_history(input);
}

static char *cli_readline_session_completion_generator(struct cli_application *ctx,
						       const char *text, int state)
{

	if (state == 0) {
		if (ctx->completion_sessions) {
			free(ctx->completion_sessions);
			ctx->completion_sessions = NULL;
		}
		ctx->completion_count = 0;
		ctx->completion_index = 0;
		ctx->completion_length = (int)strlen(text);
		if (!ctx)
			return NULL;
		(void)runtime_session_list_query(ctx->runtime, &ctx->completion_sessions,
						 &ctx->completion_count, 0, NULL);
	}

	while (ctx->completion_index < ctx->completion_count) {
		struct session *s = &ctx->completion_sessions[ctx->completion_index];
		ctx->completion_index++;
		if (s->display_id[0] &&
		    strncmp(s->display_id, text, (size_t)ctx->completion_length) == 0)
			return strdup(s->display_id);
		if (s->name[0] && strncmp(s->name, text, (size_t)ctx->completion_length) == 0)
			return strdup(s->name);
	}

	if (ctx->completion_sessions) {
		free(ctx->completion_sessions);
		ctx->completion_sessions = NULL;
	}
	ctx->completion_count = 0;
	return NULL;
}

static int cli_readline_is_session_arg_command(const char *cmd)
{
	return (strcmp(cmd, "/switch") == 0 || strcmp(cmd, "/s") == 0 ||
		strcmp(cmd, "/delete") == 0 || strcmp(cmd, "/del") == 0);
}

static char **cli_readline_cmd_completion(struct cli_application *ctx, const char *text, int start,
					  int end)
{
	(void)end;
	rl_attempted_completion_over = 1;
	if (start == 0)
		return rl_completion_matches(text, cli_command_completion_generator);

	char *cmd = strndup(rl_line_buffer, (size_t)(start - 1));
	if (!cmd)
		return NULL;
	int match = cli_readline_is_session_arg_command(cmd);
	free(cmd);
	if (match) {
		char *(*generate)(const char *, int) =
			cli_callback_generate(ctx, cli_readline_session_completion_generator);

		return generate ? rl_completion_matches(text, generate) : NULL;
	}

	return NULL;
}

#endif

const char *cli_readline_input_prompt(struct cli_application *ctx)
{
	if (!cli_output_color_enabled(ctx))
		return "> ";
	return CLI_RL_IGNORE_START CLI_INPUT_MARKER_COLOR CLI_RL_IGNORE_END
		"› " CLI_RL_IGNORE_START ANSI_RESET CLI_INPUT_BACKGROUND CLI_RL_IGNORE_END;
}

#ifdef HAVE_READLINE
void cli_readline_run(struct cli_application *ctx, struct cli_command_job *job,
		      void (*run_session)(struct cli_application *, struct cli_command_job *))
{
	using_history();
	int previous_catch_signals = rl_catch_signals;
	int previous_catch_winch = rl_catch_sigwinch;

	rl_catch_signals = 0;
	rl_catch_sigwinch = 0;
	rl_initialize();
	Keymap previous_keymap = rl_get_keymap();
	Keymap owned_keymap = cli_keymap_copy(previous_keymap);
	int (*previous_getc)(FILE *) = rl_getc_function;
	void (*previous_display)(void) = rl_redisplay_function;
	char **(*previous_completion)(const char *, int, int) = rl_attempted_completion_function;

	if (!owned_keymap) {
		rl_catch_signals = previous_catch_signals;
		rl_catch_sigwinch = previous_catch_winch;
		return;
	}
	if (cli_composer_model_init(&ctx->composer) != 0) {
		cli_keymap_free(owned_keymap);
		rl_catch_signals = previous_catch_signals;
		rl_catch_sigwinch = previous_catch_winch;
		return;
	}
	rl_set_keymap(owned_keymap);
	memset(&ctx->input_status, 0, sizeof(ctx->input_status));
	ctx->input_status.enabled = 1;
	ctx->readline_ready_input = NULL;
	int (*reader)(FILE *) = cli_callback_getc(ctx, cli_poll_getc);
	void (*display)(void) = cli_callback_display(ctx, cli_readline_redisplay);
	char **(*completion)(const char *, int, int) =
		cli_callback_complete(ctx, cli_readline_cmd_completion);
	void (*line_ready)(char *) = cli_callback_line(ctx, cli_readline_line_ready);

	cli_readline_configure(ctx);
	if (ctx->callback_error) {
		CMD_ERROR(ctx, "failed to create input callbacks: %s",
			  morph_strerror(ctx->callback_error));
		goto out_release_readline;
	}
	rl_getc_function = reader;
	rl_redisplay_function = display;
	rl_attempted_completion_function = completion;
	(void)line_ready;
	run_session(ctx, job);
out_release_readline:
	memset(&ctx->input_status, 0, sizeof(ctx->input_status));
	cli_composer_model_cleanup(&ctx->composer);
	free(ctx->completion_sessions);
	ctx->completion_sessions = NULL;
	rl_set_keymap(previous_keymap);
	cli_keymap_free(owned_keymap);
	rl_getc_function = previous_getc;
	rl_redisplay_function = previous_display;
	rl_attempted_completion_function = previous_completion;
	rl_catch_signals = previous_catch_signals;
	rl_catch_sigwinch = previous_catch_winch;
	cli_callbacks_cleanup(ctx);
}
#endif
