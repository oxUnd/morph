#include "sapi/cli/presentation/shortcuts.h"

#include <nb/buf.h>
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/utf8.h>
#include <nb/vec.h>
#include <nothing/tui/theme.h>
#include <nothing/tui/views/text_view.h>

#include <ctype.h>
#include <string.h>

static int cli_shortcuts_key(const char *text, size_t length, int action)
{
	static const char *const names[] = {
		"Ctrl", "Shift", "Alt", "Cmd", "Meta", "Super", "Enter", "Esc", "Escape",
		"Tab", "Space", "Backspace", "Up", "Down", "Left", "Right", "Home", "End",
		"Insert", "Delete", "Del", "PgUp", "PgDn", "PageUp", "PageDown",
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		size_t n = strlen(names[i]);
		if (length >= n && !memcmp(text, names[i], n) &&
		    (length == n || text[n] == '+' || text[n] == '/'))
			return 1;
	}

	uint32_t cp = 0;
	size_t used = 0;
	if (nb_utf8_read(text, length, &cp, &used) &&
	    cp >= 0x2190 && cp <= 0x2193)
		return 1;

	return action && ((length == 1 &&
		(isalpha((unsigned char)*text) || *text == '?' || *text == '/')) ||
		(length > 1 && isdigit((unsigned char)*text) &&
		 (memchr(text, '-', length) || (length >= 4 && !memcmp(text + 1, "–", 3)))));
}

static int cli_shortcuts_append(nb_vec *out, nb_buf *bytes, const char *text, size_t length,
	struct nt_text_style style, const char *hyperlink)
{
	struct nt_text_span *span = nb_vec_push(out);
	if (!span)
		return NB_ERR_NOMEM;

	*span = (struct nt_text_span){.style = style, .hyperlink = hyperlink};
	return nb_buf_append(bytes, text, length) && nb_buf_append_char(bytes, '\0')
		? 0 : NB_ERR_NOMEM;
}

static int cli_shortcuts_apply(struct nt_view *view, const struct nt_text_span *spans,
	size_t count, int styled, int first_line)
{
	if (!view || (count && !spans))
		return NB_ERR_INVAL;

	nb_vec output NB_CLEANUP(nb_vec_cleanup) = {0};
	nb_buf bytes NB_CLEANUP(nb_buf_cleanup) = {0};
	struct nt_text_span item = {0};
	nb_vec_init(&output, sizeof(item));
	struct nt_text_style accent = nt_theme_style(nt_view_theme(view), NT_THEME_ACCENT,
		NT_VISUAL_NORMAL);
	int rc = 0;
	int header = 1;
	for (size_t i = 0; !rc && i < count; i++) {
		const struct nt_text_span *source = &spans[i];
		const char *cursor = source->text ? source->text : "";
		int action = 1;
		while (!rc && *cursor) {
			if (!header) {
				rc = cli_shortcuts_append(&output, &bytes, cursor, strlen(cursor),
					source->style, source->hyperlink);
				break;
			}

			size_t gap = strspn(cursor, " \t\r\n()");
			if (gap) {
				rc = cli_shortcuts_append(&output, &bytes, cursor, gap,
					source->style, source->hyperlink);
				if (gap > 1 || memchr(cursor, '\n', gap) || cursor[gap - 1] == '(')
					action = 1;

				if (first_line && memchr(cursor, '\n', gap))
					header = 0;

				cursor += gap;
				if (!header)
					continue;
			}

			size_t length = strcspn(cursor, " \t\r\n()");
			if (!length)
				continue;

			struct nt_text_style style = source->style;
			int key = cli_shortcuts_key(cursor, length, action);
			if (key) {
				style.attributes &= ~(unsigned)NT_STYLE_DIM;
				style.attributes |= NT_STYLE_BOLD;
				if (styled)
					style.foreground = accent.foreground;
			}

			if (!rc)
				rc = cli_shortcuts_append(&output, &bytes, cursor, length, style,
					source->hyperlink);

			action = key || (length == 2 && !memcmp(cursor, "·", 2)) ||
				(length == 1 && *cursor == '/');
			cursor += length;
		}
	}

	if (rc)
		return rc;

	size_t offset = 0;
	for (size_t i = 0; i < output.len; i++) {
		struct nt_text_span *span = nb_vec_at(&output, i);
		span->text = nb_buf_cstr(&bytes) + offset;
		offset += strlen(span->text) + 1;
	}

	return nt_text_view_set_spans(view, (const struct nt_text_span *)output.data, output.len);
}

int cli_shortcuts_set_spans(struct nt_view *view, const struct nt_text_span *spans,
	size_t count, int styled)
{
	return cli_shortcuts_apply(view, spans, count, styled, 0);
}

int cli_shortcuts_set_header_spans(struct nt_view *view, const struct nt_text_span *spans,
	size_t count, int styled)
{
	return cli_shortcuts_apply(view, spans, count, styled, 1);
}
