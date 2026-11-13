#include "sapi/cli/presentation/output.h"
#include "nb/err.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/turn_details.h"
#include "sapi/cli/services/command_job.h"
#include "agent/turn.h"
#include "sapi/cli/adapters/input/callbacks.h"
#include "sapi/cli/adapters/input/cancel.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/services/scheduler.h"
#include "sapi/cli/services/recap.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/application_internal.h"
#include "runtime/runtime.h"
#include "sapi/cli/pages/chat.h"
#include <stdarg.h>


void cli_output_set_color_enabled(struct cli_application *ctx, int enabled)
{
	if (ctx)
		ctx->output.color_disabled = !enabled;
}

int cli_output_color_enabled(struct cli_application *ctx)
{
	return (!ctx || !ctx->output.color_disabled);
}

void cli_output_set_structured(struct cli_application *ctx, int enabled)
{
	if (ctx)
		ctx->output.structured = enabled ? 1 : 0;
}

int cli_output_is_structured(struct cli_application *ctx)
{
	return (ctx && ctx->output.structured);
}

int cli_output_overlay_open(const struct cli_application *ctx)
{
	return cli_turn_details_is_open(ctx);
}

int cli_output_overlay_visible(const struct cli_application *ctx)
{
	return cli_turn_details_is_visible(ctx);
}

int cli_output_capture_begin(struct cli_application *ctx, nb_buf *output)
{
	if (!ctx || !output || ctx->output.capture)
		return NB_ERR_INVAL;
	ctx->output.capture = output;
	return 0;
}

void cli_output_capture_end(struct cli_application *ctx)
{
	if (!ctx)
		return;
	ctx->output.capture = NULL;
	ctx->output.capture_styled = 0;
	ctx->output.capture_history = 0;
}

int cli_output_capture_write(struct cli_application *ctx, const char *bytes, size_t len)
{
	int rc;
	char *safe;

	if (!ctx || !ctx->output.capture)
		return 0;
	if (ctx->output.capture_styled) {
		rc = (nb_buf_append(ctx->output.capture, bytes, len) ? 0 : NB_ERR_NOMEM);
	} else {
		safe = morph_terminal_sanitize_dup(bytes, len, MORPH_TERMINAL_TEXT_MULTILINE, NULL);
		if (!safe)
			return NB_ERR_NOMEM;
		rc = (nb_buf_append_cstr(ctx->output.capture, safe) ? 0 : NB_ERR_NOMEM);
		free(safe);
	}
	if (!rc && ctx->output.capture_history)
		cli_output_track_tail(&ctx->output.trailing_newlines, bytes, len);
	return rc != 0 ? rc : 1;
}

/* Only for renderers whose untrusted inputs have already been sanitized. */
int cli_output_capture_styled_begin(struct cli_application *ctx, nb_buf *output)
{
	int rc = cli_output_capture_begin(ctx, output);

	if (rc == 0)
		ctx->output.capture_styled = 1;
	return rc;
}

static int cli_output_capture_text(struct cli_application *ctx, const char *text)
{
	if (!ctx || !ctx->output.capture || !text)
		return NB_ERR_INVAL;
	int rc = cli_output_capture_write(ctx, text, strlen(text));
	return rc < 0 ? rc : 0;
}

static const unsigned char *cli_output_skip_control_string(const unsigned char *cursor)
{
	while (*cursor) {
		if (*cursor == '\a')
			return cursor + 1;
		if (*cursor == 0x1bu && cursor[1] == '\\')
			return cursor + 2;
		cursor++;
	}
	return cursor;
}

static int cli_output_write_visible_control(unsigned cp)
{
	if (cp == '\a')
		return fputs("\\a", stdout) == EOF ? NB_ERR_IO : 0;
	if (cp == '\b')
		return fputs("\\b", stdout) == EOF ? NB_ERR_IO : 0;
	if (cp <= 0xffu)
		return fprintf(stdout, "\\x%02x", cp) < 0 ? NB_ERR_IO : 0;
	return fprintf(stdout, "\\u%04x", cp) < 0 ? NB_ERR_IO : 0;
}

static int cli_output_write_terminal_safe(const char *src, int allow_sgr)
{
	const unsigned char *cursor = (const unsigned char *)src;
	const unsigned char *end;

	if (!src)
		return 0;
	end = cursor + strlen(src);
	while (cursor < end) {
		if (*cursor == 0x1bu) {
			const unsigned char *start = cursor++;

			if (cursor < end && *cursor == '[') {
				cursor++;
				while (cursor < end && (*cursor < 0x40u || *cursor > 0x7eu))
					cursor++;
				if (cursor < end) {
					cursor++;
					if (allow_sgr && cursor[-1] == 'm' &&
					    fwrite(start, 1, (size_t)(cursor - start), stdout) !=
						    (size_t)(cursor - start))
						return NB_ERR_IO;
				}
				continue;
			}
			if (cursor < end && (*cursor == ']' || *cursor == 'P' || *cursor == 'X' ||
					     *cursor == '^' || *cursor == '_')) {
				cursor = cli_output_skip_control_string(cursor + 1);
				continue;
			}
			if (cursor < end)
				cursor++;
			continue;
		}
		if (*cursor < 0x20u || *cursor == 0x7fu) {
			unsigned cp = *cursor++;

			if (cp == '\n' || cp == '\t') {
				if (fputc((int)cp, stdout) == EOF)
					return NB_ERR_IO;
			} else if (cli_output_write_visible_control(cp) != 0) {
				return NB_ERR_IO;
			}
			continue;
		}
		if (*cursor >= 0x80u) {
			unsigned cp;
			size_t avail = (size_t)(end - cursor);
			size_t cp_len;

			if (!nb_utf8_read((const char *)cursor, avail, &cp, &cp_len)) {
				cursor++;
				continue;
			}
			if (cp >= 0x80u && cp <= 0x9fu) {
				if (cli_output_write_visible_control(cp) != 0)
					return NB_ERR_IO;
			} else if (fwrite(cursor, 1, cp_len, stdout) != cp_len) {
				return NB_ERR_IO;
			}
			cursor += cp_len;
			continue;
		}
		if (fputc(*cursor++, stdout) == EOF)
			return NB_ERR_IO;
	}
	return 0;
}

int cli_output_result_begin(struct cli_application *ctx)
{
	if (cli_chat_active(ctx)) {
		ctx->output.command_result_pending = 0;
		return 0;
	}
	if (!ctx || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE ||
	    !ctx->output.command_result_pending || ctx->output.capture || ctx->output.structured)
		return 0;
	ctx->output.command_result_pending = 0;
	if (fputc('\n', stdout) == EOF)
		return NB_ERR_IO;
	return 0;
}

/* Track committed text only; transient terminal frames own their own rows. */
void cli_output_track_tail(int *tail, const char *bytes, size_t len)
{
	char *safe = morph_terminal_sanitize_dup(bytes, len, MORPH_TERMINAL_TEXT_MULTILINE, NULL);
	int newlines = 0;
	size_t end;

	if (!safe)
		return;
	end = strlen(safe);
	while (end && strchr(" \t\r\n", safe[end - 1])) {
		if (safe[--end] == '\n' && newlines < 2)
			newlines++;
	}
	if (end)
		*tail = newlines;
	else {
		*tail += newlines;
		if (*tail > 2)
			*tail = 2;
	}
	free(safe);
}

int cli_output_block_gap_margin(struct morph_tui_view_insets margin, int trailing_newlines)
{
	struct morph_tui_view_rect slot = {
		.x = 0,
		.y = 0,
		.width = 1,
		.height = margin.top + 1,
	};
	struct morph_tui_view_rect frame = morph_tui_view_inset_rect(slot, margin);
	int required = frame.y + 1;

	/* Nothing precedes the first visible block, so it starts on the next
	 * line rather than after a blank row. */
	if (trailing_newlines <= 0)
		return 1;
	if (trailing_newlines >= required)
		return 0;
	return required - trailing_newlines;
}

int cli_output_block_append(nb_buf *out, const char *text, struct morph_tui_view_insets margin,
			    int *trailing_newlines)
{
	size_t start;
	int gap;
	int rc;

	if (!out || !text || !trailing_newlines)
		return NB_ERR_INVAL;
	if (!text[0])
		return 0;
	gap = cli_output_block_gap_margin(margin, *trailing_newlines);
	for (int i = 0; i < gap; i++) {
		rc = (nb_buf_append_char(out, '\n') ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
	}
	start = out->len;
	rc = (nb_buf_append_cstr(out, text) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	if (out->len && out->ptr[out->len - 1] != '\n') {
		rc = (nb_buf_append_char(out, '\n') ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
	}
	cli_output_track_tail(trailing_newlines, out->ptr + start, out->len - start);
	return 0;
}

int cli_output_block_write(struct cli_application *ctx, const char *text,
			   struct morph_tui_view_insets margin)
{
	nb_buf document NB_CLEANUP(nb_buf_cleanup) = {0};
	int trailing;
	int rc;

	if (!ctx || !text)
		return NB_ERR_INVAL;
	if (!text[0])
		return 0;
	rc = (nb_buf_init(&document),
	      nb_buf_reserve(&document, strlen(text) + 16) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	trailing = ctx->output.trailing_newlines;
	rc = cli_output_block_append(&document, text, margin, &trailing);
	if (rc == 0)
		rc = cli_output_printf(ctx, "%s", nb_buf_cstr(&document));
	nb_buf_cleanup(&document);
	return rc;
}

/* Add the command/result gap only when a visible result actually arrives. */
static int cli_output_write_output(struct cli_application *ctx, const char *text)
{
	if (cli_chat_active(ctx))
		return cli_chat_output(ctx, text);
	if (ctx && ctx->presentation_mode == CLI_PRESENT_INTERACTIVE &&
	    ctx->output.command_result_pending) {
		while (*text == '\n' || *text == '\r')
			text++;
		if (morph_terminal_display_width_ansi(text) > 0 &&
		    cli_output_result_begin(ctx) != 0)
			return NB_ERR_IO;
	}
	int rc = cli_output_write_terminal_safe(text, !ctx || !ctx->output.color_disabled);
	if (!rc && ctx && !cli_turn_details_is_visible(ctx)) {
		cli_output_track_tail(&ctx->output.trailing_newlines, text, strlen(text));
		cli_feed_history_advance(ctx, text, strlen(text));
	}
	return rc;
}

int cli_output_printf(struct cli_application *ctx, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	va_list ap_size;
	va_copy(ap_size, ap);
	n = vsnprintf(NULL, 0, fmt, ap_size);
	va_end(ap_size);
	if (n < 0) {
		va_end(ap);
		return n;
	}
	char *buf = malloc((size_t)n + 1);
	if (!buf) {
		va_end(ap);
		return NB_ERR_NOMEM;
	}
	vsnprintf(buf, (size_t)n + 1, fmt, ap);
	va_end(ap);
	if (ctx && ctx->output.capture) {
		int rc = cli_output_capture_text(ctx, buf);

		free(buf);
		if (rc != 0)
			return rc;
		return n;
	}
	if ((ctx && ctx->output.structured)) {
		free(buf);
		return n;
	}
	if (cli_output_write_output(ctx, buf) != 0) {
		free(buf);
		return NB_ERR_IO;
	}
	free(buf);
	return n;
}

int cli_output_fputs(struct cli_application *ctx, const char *text, FILE *stream)
{
	int rc;

	if (!text || !stream)
		return NB_ERR_INVAL;
	if (stream != stdout)
		return fputs(text, stream);
	if (ctx && ctx->output.capture) {
		rc = cli_output_capture_text(ctx, text);
		return rc == 0 ? 0 : EOF;
	}
	if ((ctx && ctx->output.structured))
		return 0;
	return cli_output_write_output(ctx, text) == 0 ? 0 : EOF;
}

int cli_output_putchar(struct cli_application *ctx, int ch)
{
	char text[2] = {(char)ch, '\0'};

	if (ctx && ctx->output.capture)
		return cli_output_capture_text(ctx, text) == 0 ? ch : EOF;
	if ((ctx && ctx->output.structured))
		return ch;
	if (cli_chat_active(ctx))
		return cli_chat_output(ctx, text) == 0 ? ch : EOF;
	if (ctx && ctx->presentation_mode == CLI_PRESENT_INTERACTIVE &&
	    ctx->output.command_result_pending) {
		if (ch == '\n' || ch == '\r')
			return ch;
		if (cli_output_result_begin(ctx) != 0)
			return EOF;
	}
	int rc = fputc(ch, stdout);
	if (rc != EOF && ctx && !cli_turn_details_is_visible(ctx)) {
		cli_output_track_tail(&ctx->output.trailing_newlines, text, 1);
		cli_feed_history_advance(ctx, text, 1);
	}
	return rc;
}

int cli_output_write_json(const char *json)
{
	size_t len;

	if (!json)
		return NB_ERR_INVAL;
	len = strlen(json);
	if (fwrite(json, 1, len, stdout) != len || fputc('\n', stdout) == EOF)
		return NB_ERR_IO;
	fflush(stdout);
	return 0;
}

int cli_output_print_untrusted(struct cli_application *ctx, const char *text,
			       enum morph_terminal_text_mode mode)
{
	char *safe;
	int rc = 0;

	if (!text)
		return 0;
	safe = morph_terminal_sanitize_dup(text, strlen(text), mode, NULL);
	if (!safe)
		return NB_ERR_NOMEM;
	if (cli_output_fputs(ctx, safe, stdout) == EOF)
		rc = NB_ERR_IO;
	free(safe);
	if (rc != 0)
		return rc;
	return 0;
}

void cli_output_print_padded(struct cli_application *ctx, const char *s, int target_width)
{
	char *safe;
	size_t width;
	int dw;
	int pad;

	if (!s)
		s = "";
	safe = morph_terminal_sanitize_dup(s, strlen(s), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return;
	width = nb_utf8_width_cstr(safe);
	dw = width > (size_t)INT_MAX ? INT_MAX : (int)width;
	(void)cli_output_fputs(ctx, safe, stdout);
	free(safe);
	pad = target_width - dw;
	for (int i = 0; i < pad; i++)
		(void)cli_output_putchar(ctx, ' ');
}
