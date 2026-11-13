#include <morph/tui/widgets/editor.h>
#include <morph/tui/widgets/list.h>
#include "util/error.h"
#include "util/utf8.h"
#include "util/cleanup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void widget_editor_wipe(void *data, size_t length)
{
	volatile unsigned char *bytes = data;

	while (length--)
		*bytes++ = 0;
}

struct widget_editor_snapshot {
	morph_buf_t text;
	size_t cursor;
	int overflow;
};

#define WIDGET_EDITOR_UNDO_STEPS 128u
#define WIDGET_EDITOR_UNDO_BYTES (8u * 1024u * 1024u)

static void widget_editor_buffer_cleanup(morph_buf_t *text)
{
	if (text->data)
		widget_editor_wipe(text->data, text->cap);
	morph_buf_cleanup(text);
}

static void widget_editor_snapshot_cleanup(struct widget_editor_snapshot **owner)
{
	if (!owner || !*owner)
		return;
	widget_editor_buffer_cleanup(&(*owner)->text);
	free(*owner);
	*owner = NULL;
}

static struct widget_editor_snapshot *widget_editor_snapshot_steal(
	struct widget_editor_snapshot **owner)
{
	struct widget_editor_snapshot *snapshot = *owner;
	*owner = NULL;
	return snapshot;
}

static int widget_editor_save(struct morph_tui_widget_editor_state *editor)
{
	if (editor->secret || editor->pasting || utf8valid(editor->text.data) ||
	    editor->text.len >= WIDGET_EDITOR_UNDO_BYTES)
		return 0;
	struct widget_editor_snapshot *snapshot MORPH_CLEANUP(widget_editor_snapshot_cleanup) =
		calloc(1, sizeof(*snapshot));
	if (!snapshot)
		return -ENOMEM;
	int rc = morph_buf_init(&snapshot->text, editor->text.len + 1);
	if (!rc)
		rc = morph_buf_puts(&snapshot->text, editor->text.data);
	if (rc)
		return rc;
	snapshot->cursor = editor->cursor;
	snapshot->overflow = editor->overflow;
	if (!editor->undo.elts)
		rc = morph_array_init(&editor->undo, 16, sizeof(snapshot));
	if (rc)
		return rc;
	struct widget_editor_snapshot **slot = morph_array_push_borrow(&editor->undo);
	if (!slot)
		return -ENOMEM;
	editor->undo_bytes += snapshot->text.cap;
	*slot = widget_editor_snapshot_steal(&snapshot);
	while (editor->undo.nelts > WIDGET_EDITOR_UNDO_STEPS ||
	       editor->undo_bytes > WIDGET_EDITOR_UNDO_BYTES) {
		struct widget_editor_snapshot **first = morph_array_borrow(&editor->undo, 0);
		editor->undo_bytes -= (*first)->text.cap;
		widget_editor_snapshot_cleanup(first);
		memmove(first, first + 1, (editor->undo.nelts - 1) * sizeof(*first));
		morph_array_pop(&editor->undo);
	}
	return 0;
}

static int widget_editor_boundary(const struct morph_tui_widget_editor_state *editor,
				  size_t offset)
{
	return offset <= editor->text.len && utf8_clamp_bytes(editor->text.data, offset) == offset;
}

int morph_tui_widget_editor_state_set_cursor(struct morph_tui_widget_editor_state *editor,
						 size_t offset)
{
	if (!editor || !editor->text.data)
		return -EINVAL;
	if (utf8valid(editor->text.data))
		return MORPH_ERR_FORMAT;
	if (!widget_editor_boundary(editor, offset))
		return -EINVAL;
	editor->cursor = offset;
	return 0;
}

int morph_tui_widget_editor_state_replace_range_copy(struct morph_tui_widget_editor_state *editor,
						     size_t start, size_t end,
						     const char *replacement)
{
	if (!editor || !editor->text.data || !replacement || start > end)
		return -EINVAL;
	if (utf8valid(editor->text.data) || utf8valid(replacement))
		return MORPH_ERR_FORMAT;
	if (!widget_editor_boundary(editor, start) || !widget_editor_boundary(editor, end))
		return -EINVAL;
	size_t length = strlen(replacement);
	size_t retained = editor->text.len - (end - start);
	if (length >= editor->limit - retained)
		return -E2BIG;
	if (length == end - start && !memcmp(editor->text.data + start, replacement, length)) {
		editor->cursor = start + length;
		return 0;
	}
	morph_buf_t next MORPH_CLEANUP(widget_editor_buffer_cleanup) = {0};
	int rc = morph_buf_init(&next, retained + length + 1);
	if (!rc)
		rc = morph_buf_append(&next, editor->text.data, start);
	if (!rc)
		rc = morph_buf_puts(&next, replacement);
	if (!rc)
		rc = morph_buf_append(&next, editor->text.data + end, editor->text.len - end);
	if (!rc)
		rc = widget_editor_save(editor);
	if (rc)
		return rc;
	widget_editor_wipe(editor->text.data, editor->text.cap);
	morph_buf_move(&editor->text, &next);
	editor->cursor = start + length;
	editor->overflow = 0;
	return 0;
}

int morph_tui_widget_editor_state_undo(struct morph_tui_widget_editor_state *editor)
{
	if (!editor || !editor->text.data)
		return -EINVAL;
	if (editor->secret || !editor->undo.nelts)
		return MORPH_TUI_WIDGET_UNHANDLED;
	struct widget_editor_snapshot **slot =
		morph_array_borrow(&editor->undo, editor->undo.nelts - 1);
	struct widget_editor_snapshot *snapshot MORPH_CLEANUP(widget_editor_snapshot_cleanup) =
		widget_editor_snapshot_steal(slot);
	morph_array_pop(&editor->undo);
	editor->undo_bytes -= snapshot->text.cap;
	widget_editor_wipe(editor->text.data, editor->text.cap);
	morph_buf_move(&editor->text, &snapshot->text);
	editor->cursor = snapshot->cursor;
	editor->overflow = snapshot->overflow;
	return MORPH_TUI_WIDGET_CHANGED;
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
		int rc = widget_editor_save(editor);
		if (rc)
			return rc;
		editor->pasting = 1;
		return MORPH_TUI_WIDGET_UNHANDLED;
	}
	if (key == MORPH_TUI_KEY_PASTE_END) {
		editor->pasting = 0;
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if (key == 4 && editor->text.len)
		key = MORPH_TUI_KEY_DELETE;
	if (editor->pasting && (key < 32 || key >= 256 || key == 127))
		return MORPH_TUI_WIDGET_UNHANDLED;
	if (key == 31)
		return morph_tui_widget_editor_state_undo(editor);
	if (key == 3 || key == 4)
		return MORPH_TUI_WIDGET_CANCEL;
	if (key == 27)
		return MORPH_TUI_WIDGET_BACK;
	if (key == '\r' || key == '\n')
		return MORPH_TUI_WIDGET_SUBMIT;
	morph_buf_t *text = &editor->text;

	if (key == 21) {
		int rc = text->len ? widget_editor_save(editor) : 0;
		if (rc)
			return rc;
		widget_editor_wipe(text->data, text->cap);
		morph_buf_reset(text);
		editor->cursor = 0;
		editor->overflow = 0;
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if (key == 11) {
		int rc = editor->cursor < text->len ? widget_editor_save(editor) : 0;
		if (rc)
			return rc;
		widget_editor_wipe(text->data + editor->cursor, text->len - editor->cursor);
		text->len = editor->cursor;
		text->data[text->len] = '\0';
		editor->overflow = 0;
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if (key == 23 && editor->cursor) {
		size_t start = editor->cursor;
		int word = 0;
		while (start) {
			size_t previous = (size_t)(utf8_prev_codepoint_borrow(text->data,
				text->data + start) - text->data);
			unsigned cp = 0;
			size_t bytes = 0;
			if (!utf8_decode_codepoint(text->data + previous, start - previous,
						    &cp, &bytes))
				return MORPH_ERR_FORMAT;
			int space = utf8_is_unicode_space_cp(cp);
			if (word && space)
				break;
			word |= !space;
			start = previous;
		}
		int rc = widget_editor_save(editor);
		if (rc)
			return rc;
		memmove(text->data + start, text->data + editor->cursor,
			text->len - editor->cursor + 1);
		text->len -= editor->cursor - start;
		editor->cursor = start;
		editor->overflow = 0;
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if ((key == 127 || key == 8) && editor->cursor) {
		size_t previous = (size_t)(utf8_prev_codepoint_borrow(text->data,
								      text->data + editor->cursor) -
					   text->data);
		int rc = widget_editor_save(editor);
		if (rc)
			return rc;
		memmove(text->data + previous, text->data + editor->cursor,
			text->len - editor->cursor + 1);
		text->len -= editor->cursor - previous;
		editor->cursor = previous;
		editor->overflow = 0;
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if ((key == MORPH_TUI_KEY_LEFT || key == 2) && editor->cursor) {
		editor->cursor = (size_t)(utf8_prev_codepoint_borrow(text->data,
								     text->data + editor->cursor) -
					  text->data);
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if ((key == MORPH_TUI_KEY_RIGHT || key == 6) && editor->cursor < text->len) {
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
	if (key == MORPH_TUI_KEY_DELETE) {
		if (editor->cursor >= text->len)
			return MORPH_TUI_WIDGET_CHANGED;
		size_t bytes = utf8_next_codepoint_len(text->data + editor->cursor,
			text->len - editor->cursor);
		int rc = widget_editor_save(editor);
		if (rc)
			return rc;
		memmove(text->data + editor->cursor, text->data + editor->cursor + bytes,
			text->len - editor->cursor - bytes + 1);
		text->len -= bytes;
		editor->overflow = 0;
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if (key < 32 || key >= 256 || key == 127)
		return MORPH_TUI_WIDGET_UNHANDLED;
	if (text->len >= editor->limit - 1) {
		editor->overflow = 1;
		return MORPH_TUI_WIDGET_CHANGED;
	}
	int rc = morph_buf_reserve(text, 2);

	if (!rc)
		rc = widget_editor_save(editor);
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
	for (size_t i = 0; i < editor->undo.nelts; i++) {
		struct widget_editor_snapshot **slot = morph_array_borrow(&editor->undo, i);
		widget_editor_snapshot_cleanup(slot);
	}
	morph_array_cleanup(&editor->undo);
	*editor = (struct morph_tui_widget_editor_state){0};
}

void morph_tui_widget_editor_state_move(struct morph_tui_widget_editor_state *destination,
					struct morph_tui_widget_editor_state *source)
{
	if (!destination || !source || destination == source)
		return;
	morph_tui_widget_editor_state_cleanup(destination);
	*destination = *source;
	*source = (struct morph_tui_widget_editor_state){0};
}
