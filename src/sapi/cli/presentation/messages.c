#include "sapi/cli/presentation/messages.h"

#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "util/cleanup.h"
#include "util/utf8.h"
#include <errno.h>
#include <limits.h>
#include <string.h>

struct cli_messages_color {
	int code;
	int mode;
	int values[3];
};

struct cli_messages_sgr {
	unsigned attributes;
	struct cli_messages_color foreground;
	struct cli_messages_color background;
};

static void cli_messages_sgr_apply(struct cli_messages_sgr *state, const int *values, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		int code = values[i];
		if (code == 0) {
			memset(state, 0, sizeof(*state));
		} else if (code >= 1 && code <= 9 && code != 6 && code != 8) {
			state->attributes |= 1u << (unsigned)code;
		} else if (code == 22) {
			state->attributes &= ~((1u << 1u) | (1u << 2u));
		} else if (code == 23 || code == 24 || code == 25 || code == 27 || code == 29) {
			state->attributes &= ~(1u << (unsigned)(code - 20));
		} else if (code == 39 || code == 49) {
			struct cli_messages_color *color =
				code == 39 ? &state->foreground : &state->background;
			memset(color, 0, sizeof(*color));
		} else if ((code >= 30 && code <= 37) || (code >= 90 && code <= 97)) {
			state->foreground = (struct cli_messages_color){.code = code};
		} else if ((code >= 40 && code <= 47) || (code >= 100 && code <= 107)) {
			state->background = (struct cli_messages_color){.code = code};
		} else if ((code == 38 || code == 48) && i + 1 < count) {
			struct cli_messages_color *color =
				code == 38 ? &state->foreground : &state->background;
			int mode = values[++i];
			size_t components = mode == 5 ? 1u : mode == 2 ? 3u : 0u;
			if (!components || components > count - i - 1)
				continue;
			*color = (struct cli_messages_color){.code = code, .mode = mode};
			for (size_t j = 0; j < components; j++)
				color->values[j] = values[++i] <= 255 ? values[i] : 255;
		}
	}
}

static int cli_messages_sgr_color(morph_buf_t *out, const struct cli_messages_color *color)
{
	if (!color->code)
		return 0;
	if (color->mode == 5)
		return morph_buf_printf(out, "\033[%d;5;%dm", color->code, color->values[0]);
	if (color->mode == 2)
		return morph_buf_printf(out, "\033[%d;2;%d;%d;%dm", color->code, color->values[0],
					color->values[1], color->values[2]);
	return morph_buf_printf(out, "\033[%dm", color->code);
}

static int cli_messages_sgr_write(morph_buf_t *out, const struct cli_messages_sgr *state)
{
	int rc = morph_buf_puts(out, ANSI_RESET);
	for (unsigned code = 1; rc == 0 && code <= 9; code++)
		if (state->attributes & (1u << code))
			rc = morph_buf_printf(out, "\033[%um", code);
	if (rc == 0)
		rc = cli_messages_sgr_color(out, &state->foreground);
	if (rc == 0)
		rc = cli_messages_sgr_color(out, &state->background);
	return rc;
}

static size_t cli_messages_escape(const char *bytes, size_t length, struct cli_messages_sgr *state)
{
	if (length < 2)
		return length;
	if (bytes[1] == '[') {
		size_t end = 2;
		while (end < length &&
		       ((unsigned char)bytes[end] < 0x40u || (unsigned char)bytes[end] > 0x7eu))
			end++;
		if (end == length)
			return length;
		if (bytes[end] == 'm') {
			int values[16] = {0};
			size_t count = 0;
			int number = 0;
			int digits = 0;
			int valid = 1;
			for (size_t i = 2; i <= end && valid; i++) {
				if (bytes[i] >= '0' && bytes[i] <= '9') {
					int digit = bytes[i] - '0';
					valid = number <= (INT_MAX - digit) / 10;
					if (valid)
						number = number * 10 + digit;
					digits = 1;
				} else if (bytes[i] == ';' || bytes[i] == ':' || i == end) {
					if (digits || (i == end && count == 0)) {
						valid = count < sizeof(values) / sizeof(values[0]);
						if (valid)
							values[count++] = number;
					}
					number = 0;
					digits = 0;
				} else {
					valid = 0;
				}
			}
			if (valid)
				cli_messages_sgr_apply(state, values, count);
		}
		return end + 1;
	}
	if (strchr("]PX^_", bytes[1])) {
		for (size_t i = 2; i < length; i++) {
			if (bytes[i] == '\a')
				return i + 1;
			if (bytes[i] == '\033' && i + 1 < length && bytes[i + 1] == '\\')
				return i + 2;
		}
		return length;
	}
	return 2;
}

int cli_messages_append_rendered(morph_buf_t *out, const char *bytes, size_t length, int columns,
				 int styled)
{
	if (!out || (!bytes && length))
		return -EINVAL;
	struct cli_messages_sgr state = {0};
	size_t column = 0;
	int started = 0;
	int rc = 0;
	size_t width = columns > 0 ? (size_t)columns : 1u;
	for (size_t offset = 0; rc == 0 && offset < length;) {
		if (bytes[offset] == '\033') {
			offset += cli_messages_escape(bytes + offset, length - offset, &state);
			if (styled && started)
				rc = cli_messages_sgr_write(out, &state);
			continue;
		}
		unsigned cp = 0;
		size_t step = 0;
		if (!utf8_decode_codepoint(bytes + offset, length - offset, &cp, &step)) {
			offset++;
			continue;
		}
		if (cp == '\n') {
			if (styled)
				rc = morph_buf_puts(out, ANSI_RESET);
			if (rc == 0)
				rc = morph_buf_putc(out, '\n');
			column = 0;
			started = 0;
		} else if (cp == '\t' || (cp >= 0x20u && !(cp >= 0x7fu && cp <= 0x9fu))) {
			int cell_width = cp == '\t' ? 1 : utf8_codepoint_width(cp);
			size_t cells = cell_width > 0 ? (size_t)cell_width : 0u;
			if (column && cells > width - column) {
				if (styled)
					rc = morph_buf_puts(out, ANSI_RESET);
				if (rc == 0)
					rc = morph_buf_putc(out, '\n');
				column = 0;
				started = 0;
			}
			if (rc == 0 && styled && !started)
				rc = cli_messages_sgr_write(out, &state);
			if (rc == 0)
				rc = cp == '\t' ? morph_buf_putc(out, ' ')
						: morph_buf_append(out, bytes + offset, step);
			column += cells;
			if (column > width)
				column = width;
			started = 1;
		}
		offset += step;
	}
	if (rc == 0 && styled && started)
		rc = morph_buf_puts(out, ANSI_RESET);
	return rc;
}

static int cli_messages_spaces(morph_buf_t *out, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		int rc = morph_buf_putc(out, ' ');
		if (rc != 0)
			return rc;
	}
	return 0;
}

static int cli_messages_user_row(morph_buf_t *out, const char *text, size_t length, int first,
				 size_t width, const struct cli_message_style *style)
{
	const char *marker = style->ultra ? "» " : style->styled ? "› " : "> ";
	size_t prefix = 2u;
	int rc = style->styled ? morph_buf_puts(out, ANSI_RESET CLI_INPUT_BACKGROUND) : 0;
	if (rc == 0 && first && style->styled)
		rc = morph_buf_puts(out,
				    style->ultra ? CLI_INPUT_ULTRA_COLOR : CLI_INPUT_MARKER_COLOR);
	if (rc == 0)
		rc = first ? morph_buf_puts(out, marker) : cli_messages_spaces(out, prefix);
	if (rc == 0 && first && style->styled)
		rc = morph_buf_puts(out, ANSI_RESET CLI_INPUT_BACKGROUND);
	if (rc == 0)
		rc = morph_buf_append(out, text, length);
	if (rc == 0 && style->styled) {
		char *line MORPH_CLEANUP(morph_charp_cleanup) = strndup(text, length);
		if (!line)
			return -ENOMEM;
		size_t used = prefix + utf8_display_width(line);
		rc = cli_messages_spaces(out, used < width ? width - used : 0);
		if (rc == 0)
			rc = morph_buf_puts(out, ANSI_RESET);
	}
	return rc != 0 ? rc : morph_buf_putc(out, '\n');
}

static int cli_messages_user(morph_buf_t *out, char *safe, const struct cli_message_style *style)
{
	size_t width = style->columns > 0 ? (size_t)style->columns : 1u;
	size_t content_width = width > 1 ? width - 1 : 1u;
	size_t prefix = 2u;
	size_t body_width = content_width > prefix ? content_width - prefix : 1u;
	int rc = style->trailing_newlines < 2 ? morph_buf_putc(out, '\n') : 0;
	if (rc == 0 && style->styled) {
		rc = morph_buf_puts(out, ANSI_RESET CLI_INPUT_BACKGROUND);
		if (rc == 0)
			rc = cli_messages_spaces(out, width);
		if (rc == 0)
			rc = morph_buf_puts(out, ANSI_RESET "\n");
	}
	int first = 1;
	for (char *line = safe; line && rc == 0;) {
		char *newline = strchr(line, '\n');
		if (newline)
			*newline = '\0';
		const char *cursor = line;
		do {
			const char *end = utf8_advance_display_width_borrow(cursor, body_width);
			if (end == cursor && *cursor)
				end += utf8_next_codepoint_len(cursor, strnlen(cursor, 4));
			rc = cli_messages_user_row(out, cursor, (size_t)(end - cursor), first,
						   width, style);
			first = 0;
			cursor = end;
		} while (*cursor && rc == 0);
		line = newline ? newline + 1 : NULL;
	}
	if (rc == 0 && style->styled) {
		rc = morph_buf_puts(out, ANSI_RESET CLI_INPUT_BACKGROUND);
		if (rc == 0)
			rc = cli_messages_spaces(out, width);
		if (rc == 0)
			rc = morph_buf_puts(out, ANSI_RESET "\n");
	}
	return rc != 0 ? rc : morph_buf_putc(out, '\n');
}

int cli_messages_append(morph_buf_t *out, enum cli_message_kind kind, const char *text,
			struct cli_message_style *style)
{
	if (!out || !style || kind < CLI_MESSAGE_USER || kind > CLI_MESSAGE_ASSISTANT)
		return -EINVAL;
	if (!text || !*text)
		return 0;
	char *safe MORPH_CLEANUP(morph_charp_cleanup) =
		utf8_terminal_sanitize_dup(text, strlen(text), UTF8_TERMINAL_TEXT_MULTILINE, NULL);
	morph_buf_t document MORPH_CLEANUP(morph_buf_cleanup) = {0};
	if (!safe)
		return -ENOMEM;
	int rc = morph_buf_init(&document, 256);
	if (rc != 0)
		return rc;
	for (char *p = safe; *p; p++)
		if (*p == '\t')
			*p = ' ';
	if (kind == CLI_MESSAGE_USER) {
		rc = cli_messages_user(&document, safe, style);
	} else {
		if (style->trailing_newlines < 2)
			rc = morph_buf_putc(&document, '\n');
		if (rc == 0 && kind == CLI_MESSAGE_REASONING) {
			if (!style->reasoning_expanded) {
				const char *start = safe + strspn(safe, " \t\r\n");
				if (!*start)
					return 0;
				size_t width =
					style->columns > 4 ? (size_t)(style->columns - 4) : 1u;
				const char *end = utf8_advance_display_width_borrow(
					start, width > 1 ? width - 1 : 1u);
				const char *newline = strchr(start, '\n');
				if (newline && newline < end)
					end = newline;
				rc = morph_buf_printf(&document, "%s%.*s%s%s\n",
						      style->styled ? ANSI_DIM "\033[3m" : "",
						      (int)(end - start), start, *end ? "…" : "",
						      style->styled ? ANSI_RESET : "");
			} else {
				rc = morph_buf_printf(&document, "%s%s%s\n",
						      style->styled ? ANSI_DIM "\033[3m" : "", safe,
						      style->styled ? ANSI_RESET : "");
			}
		}
		if (rc == 0 && kind == CLI_MESSAGE_ASSISTANT) {
			rc = morph_buf_printf(&document, "%s●%s ", style->styled ? ANSI_BOLD : "",
					      style->styled ? ANSI_RESET : "");
			if (rc == 0)
				rc = cli_markdown_render_document(&document, safe, style->columns,
								  2u);
			int tail = 0;
			cli_output_track_tail(&tail, morph_buf_cstr_borrow(&document),
					      document.len);
			while (rc == 0 && tail++ < 2)
				rc = morph_buf_putc(&document, '\n');
		}
	}
	size_t start = out->len;
	int columns = kind == CLI_MESSAGE_USER ? style->columns :
		style->columns > 1 ? style->columns - 1 : 1;
	if (rc == 0)
		rc = cli_messages_append_rendered(
			out, morph_buf_cstr_borrow(&document), document.len,
			columns > 0 ? columns : 1, style->styled);
	if (rc == 0)
		cli_output_track_tail(&style->trailing_newlines, out->data + start,
				      out->len - start);
	return rc;
}
