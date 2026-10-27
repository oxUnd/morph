#include "tui/components.h"
#include "util/error.h"
#include "util/utf8.h"

#include <string.h>

static void wipe(void *data, size_t length)
{
	volatile unsigned char *bytes = data;

	while (length--)
		*bytes++ = 0;
}

int tui_editor_init(struct tui_editor *editor, const char *text,
		    size_t limit, int secret)
{
	if (!editor || !text || !limit)
		MORPH_RETURN(-EINVAL);
	if (editor->text.data)
		MORPH_RETURN(-EBUSY);
	if (strlen(text) >= limit)
		MORPH_RETURN(-E2BIG);
	if (utf8valid(text))
		MORPH_RETURN(MORPH_ERR_FORMAT);
	*editor = (struct tui_editor){.limit = limit, .secret = secret};
	int rc = morph_buf_init(&editor->text, limit < BUFSIZ ? limit : BUFSIZ);

	if (rc == 0)
		rc = morph_buf_puts(&editor->text, text);
	if (rc != 0) {
		tui_editor_cleanup(editor);
		MORPH_RETURN(rc);
	}
	editor->cursor = editor->text.len;
	return 0;
}

int tui_editor_valid(const struct tui_editor *editor)
{
	return editor && !editor->overflow && !editor->text.failed &&
		editor->text.len && !utf8valid(morph_buf_cstr(&editor->text));
}

int tui_editor_update(struct tui_editor *editor, int key)
{
	if (!editor || !editor->text.data)
		MORPH_RETURN(-EINVAL);
	if (key == TUI_KEY_PASTE_BEGIN) {
		editor->pasting = 1;
		return TUI_UNHANDLED;
	}
	if (key == TUI_KEY_PASTE_END) {
		editor->pasting = 0;
		return TUI_CHANGED;
	}
	if (editor->pasting && (key < 32 || key >= 256 || key == 127))
		return TUI_UNHANDLED;
	if (key == 3 || key == 4) return TUI_CANCEL;
	if (key == 27) return TUI_BACK;
	if (key == '\r' || key == '\n') return TUI_SUBMIT;
	morph_buf_t *text = &editor->text;

	if (key == 21) {
		wipe(text->data, text->cap);
		morph_buf_reset(text);
		editor->cursor = 0;
		editor->overflow = 0;
		return TUI_CHANGED;
	}
	if ((key == 127 || key == 8) && editor->cursor) {
		size_t previous = (size_t)(utf8_prev_codepoint(text->data,
			text->data + editor->cursor) - text->data);

		memmove(text->data + previous, text->data + editor->cursor,
			text->len - editor->cursor + 1);
		text->len -= editor->cursor - previous;
		editor->cursor = previous;
		editor->overflow = 0;
		return TUI_CHANGED;
	}
	if (key == TUI_KEY_LEFT && editor->cursor) {
		editor->cursor = (size_t)(utf8_prev_codepoint(text->data,
			text->data + editor->cursor) - text->data);
		return TUI_CHANGED;
	}
	if (key == TUI_KEY_RIGHT && editor->cursor < text->len) {
		editor->cursor += utf8_next_codepoint_len(text->data + editor->cursor,
			text->len - editor->cursor);
		return TUI_CHANGED;
	}
	if (key == TUI_KEY_HOME || key == 1) {
		editor->cursor = 0;
		return TUI_CHANGED;
	}
	if (key == TUI_KEY_END || key == 5) {
		editor->cursor = text->len;
		return TUI_CHANGED;
	}
	if (key < 32 || key >= 256 || key == 127)
		return TUI_UNHANDLED;
	if (text->len >= editor->limit - 1) {
		editor->overflow = 1;
		return TUI_CHANGED;
	}
	int rc = morph_buf_reserve(text, 1);

	if (rc != 0)
		MORPH_RETURN(rc);
	memmove(text->data + editor->cursor + 1, text->data + editor->cursor,
		text->len - editor->cursor + 1);
	text->data[editor->cursor++] = (char)key;
	text->len++;
	return TUI_CHANGED;
}

int tui_editor_view(struct tui_editor *editor, morph_buf_t *text,
		    size_t width, size_t *cursor_column)
{
	if (!editor || !text || !cursor_column || !editor->text.data)
		MORPH_RETURN(-EINVAL);
	*cursor_column = 0;
	if (!width)
		return 0;
	if (editor->secret) {
		const char *mask = editor->text.len ? "••••••••" : "";
		size_t visible = editor->text.len ? (width < 8 ? width : 8) : 0;

		*cursor_column = visible;
		return morph_buf_append(text, mask, visible * strlen("•"));
	}
	char *data = editor->text.data;
	char saved = data[editor->cursor];

	data[editor->cursor] = '\0';
	const char *start = utf8_suffix_display_width(data, width - 1);

	*cursor_column = utf8_display_width(start);
	data[editor->cursor] = saved;
	const char *end = utf8_advance_display_width(start, width);

	return morph_buf_append(text, start, (size_t)(end - start));
}

void tui_editor_cleanup(struct tui_editor *editor)
{
	if (!editor)
		return;
	if (editor->text.data)
		wipe(editor->text.data, editor->text.cap);
	morph_buf_cleanup(&editor->text);
	*editor = (struct tui_editor){0};
}
