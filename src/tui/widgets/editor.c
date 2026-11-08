#include <morph/tui/widgets/editor.h>
#include <morph/tui/widgets/list.h>
#include "util/error.h"
#include "util/utf8.h"

#include <stdio.h>
#include <string.h>

static void widget_editor_wipe(void *data, size_t length)
{
	volatile unsigned char *bytes = data;

	while (length--)
		*bytes++ = 0;
}

int morph_tui_widget_editor_state_init(struct morph_tui_widget_editor_state *editor,
				       const char *text, size_t limit, int secret)
{
	if (!editor || !text || !limit)
		return -EINVAL;
	if (editor->text.data)
		return -EBUSY;
	if (strlen(text) >= limit)
		return -E2BIG;
	if (utf8valid(text))
		return MORPH_ERR_FORMAT;
	*editor = (struct morph_tui_widget_editor_state){.limit = limit, .secret = secret};
	int rc = morph_buf_init(&editor->text, limit < BUFSIZ ? limit : BUFSIZ);

	if (rc == 0)
		rc = morph_buf_puts(&editor->text, text);
	if (rc != 0) {
		morph_tui_widget_editor_state_cleanup(editor);
		return rc;
	}
	editor->cursor = editor->text.len;
	return 0;
}

int morph_tui_widget_editor_state_valid(const struct morph_tui_widget_editor_state *editor)
{
	return editor && !editor->overflow && !editor->text.failed && editor->text.len &&
	       !utf8valid(morph_buf_cstr_borrow(&editor->text));
}

int morph_tui_widget_editor_state_update(struct morph_tui_widget_editor_state *editor, int key)
{
	if (!editor || !editor->text.data)
		return -EINVAL;
	if (key == MORPH_TUI_KEY_PASTE_BEGIN) {
		editor->pasting = 1;
		return MORPH_TUI_WIDGET_UNHANDLED;
	}
	if (key == MORPH_TUI_KEY_PASTE_END) {
		editor->pasting = 0;
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if (editor->pasting && (key < 32 || key >= 256 || key == 127))
		return MORPH_TUI_WIDGET_UNHANDLED;
	if (key == 3 || key == 4)
		return MORPH_TUI_WIDGET_CANCEL;
	if (key == 27)
		return MORPH_TUI_WIDGET_BACK;
	if (key == '\r' || key == '\n')
		return MORPH_TUI_WIDGET_SUBMIT;
	morph_buf_t *text = &editor->text;

	if (key == 21) {
		widget_editor_wipe(text->data, text->cap);
		morph_buf_reset(text);
		editor->cursor = 0;
		editor->overflow = 0;
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if ((key == 127 || key == 8) && editor->cursor) {
		size_t previous = (size_t)(utf8_prev_codepoint_borrow(text->data,
								      text->data + editor->cursor) -
					   text->data);

		memmove(text->data + previous, text->data + editor->cursor,
			text->len - editor->cursor + 1);
		text->len -= editor->cursor - previous;
		editor->cursor = previous;
		editor->overflow = 0;
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if (key == MORPH_TUI_KEY_LEFT && editor->cursor) {
		editor->cursor = (size_t)(utf8_prev_codepoint_borrow(text->data,
								     text->data + editor->cursor) -
					  text->data);
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if (key == MORPH_TUI_KEY_RIGHT && editor->cursor < text->len) {
		editor->cursor += utf8_next_codepoint_len(text->data + editor->cursor,
							  text->len - editor->cursor);
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if (key == MORPH_TUI_KEY_HOME || key == 1) {
		editor->cursor = 0;
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if (key == MORPH_TUI_KEY_END || key == 5) {
		editor->cursor = text->len;
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if (key < 32 || key >= 256 || key == 127)
		return MORPH_TUI_WIDGET_UNHANDLED;
	if (text->len >= editor->limit - 1) {
		editor->overflow = 1;
		return MORPH_TUI_WIDGET_CHANGED;
	}
	int rc = morph_buf_reserve(text, 2);

	if (rc != 0)
		return rc;
	memmove(text->data + editor->cursor + 1, text->data + editor->cursor,
		text->len - editor->cursor + 1);
	text->data[editor->cursor++] = (char)key;
	text->len++;
	return MORPH_TUI_WIDGET_CHANGED;
}

int morph_tui_widget_editor_state_view(struct morph_tui_widget_editor_state *editor,
				       morph_buf_t *text, size_t width, size_t *cursor_column)
{
	if (!editor || !text || !cursor_column || !editor->text.data)
		return -EINVAL;
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
	const char *start = utf8_suffix_display_width_borrow(data, width - 1);

	*cursor_column = utf8_display_width(start);
	data[editor->cursor] = saved;
	const char *end = utf8_advance_display_width_borrow(start, width);

	return morph_buf_append(text, start, (size_t)(end - start));
}

void morph_tui_widget_editor_state_cleanup(struct morph_tui_widget_editor_state *editor)
{
	if (!editor)
		return;
	if (editor->text.data)
		widget_editor_wipe(editor->text.data, editor->text.cap);
	morph_buf_cleanup(&editor->text);
	*editor = (struct morph_tui_widget_editor_state){0};
}
