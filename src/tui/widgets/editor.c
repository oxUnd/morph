#include <morph/tui/widgets/editor.h>
#include <morph/tui/widgets/list.h>
#include "errors.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>
#include "nb/cleanup.h"

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
	nb_buf text;
	size_t cursor;
	int overflow;
};

#define WIDGET_EDITOR_UNDO_STEPS 128u
#define WIDGET_EDITOR_UNDO_BYTES (8u * 1024u * 1024u)

static void widget_editor_buffer_cleanup(nb_buf *text)
{
	if (text->ptr)
		widget_editor_wipe(text->ptr, text->cap);
	nb_buf_cleanup(text);
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
	if (editor->secret || editor->pasting ||
	    nb_utf8_validate(nb_str_from_cstr(editor->text.ptr), NULL) ||
	    editor->text.len >= WIDGET_EDITOR_UNDO_BYTES)
		return 0;
	struct widget_editor_snapshot *snapshot NB_CLEANUP(widget_editor_snapshot_cleanup) =
		calloc(1, sizeof(*snapshot));
	if (!snapshot)
		return NB_ERR_NOMEM;
	int rc = (nb_buf_init(&snapshot->text),
		  nb_buf_reserve(&snapshot->text, editor->text.len + 1) ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = (nb_buf_append_cstr(&snapshot->text, editor->text.ptr) ? 0 : NB_ERR_NOMEM);
	if (rc)
		return rc;
	snapshot->cursor = editor->cursor;
	snapshot->overflow = editor->overflow;
	if (!((void *)editor->undo.data))
		rc = (nb_vec_init(&editor->undo, sizeof(snapshot)),
		      nb_vec_reserve(&editor->undo, 16) ? 0 : NB_ERR_NOMEM);
	if (rc)
		return rc;
	struct widget_editor_snapshot **slot = nb_vec_push(&editor->undo);
	if (!slot)
		return NB_ERR_NOMEM;
	editor->undo_bytes += snapshot->text.cap;
	*slot = widget_editor_snapshot_steal(&snapshot);
	while (editor->undo.len > WIDGET_EDITOR_UNDO_STEPS ||
	       editor->undo_bytes > WIDGET_EDITOR_UNDO_BYTES) {
		struct widget_editor_snapshot **first = nb_vec_at(&editor->undo, 0);
		editor->undo_bytes -= (*first)->text.cap;
		widget_editor_snapshot_cleanup(first);
		nb_vec_remove(&editor->undo, 0);
	}
	return 0;
}

static int widget_editor_boundary(const struct morph_tui_widget_editor_state *editor,
				  size_t offset)
{
	return offset <= editor->text.len &&
	       nb_utf8_clamp_bytes(editor->text.ptr, offset) == offset;
}

int morph_tui_widget_editor_state_set_cursor(struct morph_tui_widget_editor_state *editor,
						 size_t offset)
{
	if (!editor || !editor->text.ptr)
		return NB_ERR_INVAL;
	if (nb_utf8_validate(nb_str_from_cstr(editor->text.ptr), NULL))
		return NB_ERR_FORMAT;
	if (!widget_editor_boundary(editor, offset))
		return NB_ERR_INVAL;
	editor->cursor = offset;
	return 0;
}

int morph_tui_widget_editor_state_replace_range_copy(struct morph_tui_widget_editor_state *editor,
						     size_t start, size_t end,
						     const char *replacement)
{
	if (!editor || !editor->text.ptr || !replacement || start > end)
		return NB_ERR_INVAL;
	if (nb_utf8_validate(nb_str_from_cstr(editor->text.ptr), NULL) ||
	    nb_utf8_validate(nb_str_from_cstr(replacement), NULL))
		return NB_ERR_FORMAT;
	if (!widget_editor_boundary(editor, start) || !widget_editor_boundary(editor, end))
		return NB_ERR_INVAL;
	size_t length = strlen(replacement);
	size_t retained = editor->text.len - (end - start);
	if (length >= editor->limit - retained)
		return NB_ERR_TOO_BIG;
	if (length == end - start && !memcmp(editor->text.ptr + start, replacement, length)) {
		editor->cursor = start + length;
		return 0;
	}
	nb_buf next NB_CLEANUP(widget_editor_buffer_cleanup) = {0};
	int rc = (nb_buf_init(&next),
		  nb_buf_reserve(&next, retained + length + 1) ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = (nb_buf_append(&next, editor->text.ptr, start) ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = (nb_buf_append_cstr(&next, replacement) ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = (nb_buf_append(&next, editor->text.ptr + end, editor->text.len - end)
			      ? 0
			      : NB_ERR_NOMEM);
	if (!rc)
		rc = widget_editor_save(editor);
	if (rc)
		return rc;
	widget_editor_wipe(editor->text.ptr, editor->text.cap);
	(nb_buf_destroy(&editor->text), nb_buf_move(&editor->text, &next));
	editor->cursor = start + length;
	editor->overflow = 0;
	return 0;
}

int morph_tui_widget_editor_state_undo(struct morph_tui_widget_editor_state *editor)
{
	if (!editor || !editor->text.ptr)
		return NB_ERR_INVAL;
	if (editor->secret || !editor->undo.len)
		return MORPH_TUI_WIDGET_UNHANDLED;
	struct widget_editor_snapshot **slot = nb_vec_at(&editor->undo, editor->undo.len - 1);
	struct widget_editor_snapshot *snapshot NB_CLEANUP(widget_editor_snapshot_cleanup) =
		widget_editor_snapshot_steal(slot);
	nb_vec_pop(&editor->undo);
	editor->undo_bytes -= snapshot->text.cap;
	widget_editor_wipe(editor->text.ptr, editor->text.cap);
	(nb_buf_destroy(&editor->text), nb_buf_move(&editor->text, &snapshot->text));
	editor->cursor = snapshot->cursor;
	editor->overflow = snapshot->overflow;
	return MORPH_TUI_WIDGET_CHANGED;
}

int morph_tui_widget_editor_state_init(struct morph_tui_widget_editor_state *editor,
				       const char *text, size_t limit, int secret)
{
	if (!editor || !text || !limit)
		return NB_ERR_INVAL;
	if (editor->text.ptr)
		return NB_ERR_BUSY;
	if (strlen(text) >= limit)
		return NB_ERR_TOO_BIG;
	if (nb_utf8_validate(nb_str_from_cstr(text), NULL))
		return NB_ERR_FORMAT;
	*editor = (struct morph_tui_widget_editor_state){.limit = limit, .secret = secret};
	int rc =
		(nb_buf_init(&editor->text),
		 nb_buf_reserve(&editor->text, limit < BUFSIZ ? limit : BUFSIZ) ? 0 : NB_ERR_NOMEM);

	if (rc == 0)
		rc = (nb_buf_append_cstr(&editor->text, text) ? 0 : NB_ERR_NOMEM);
	if (rc != 0) {
		morph_tui_widget_editor_state_cleanup(editor);
		return rc;
	}
	editor->cursor = editor->text.len;
	return 0;
}

int morph_tui_widget_editor_state_valid(const struct morph_tui_widget_editor_state *editor)
{
	return editor && !editor->overflow && !nb_buf_status(&editor->text) && editor->text.len &&
	       !nb_utf8_validate(nb_str_from_cstr(nb_buf_cstr(&editor->text)), NULL);
}

int morph_tui_widget_editor_state_update(struct morph_tui_widget_editor_state *editor, int key)
{
	if (!editor || !editor->text.ptr)
		return NB_ERR_INVAL;
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
	nb_buf *text = &editor->text;

	if (key == 21) {
		int rc = text->len ? widget_editor_save(editor) : 0;
		if (rc)
			return rc;
		widget_editor_wipe(text->ptr, text->cap);
		nb_buf_clear(text);
		editor->cursor = 0;
		editor->overflow = 0;
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if (key == 11) {
		int rc = editor->cursor < text->len ? widget_editor_save(editor) : 0;
		if (rc)
			return rc;
		widget_editor_wipe(text->ptr + editor->cursor, text->len - editor->cursor);
		text->len = editor->cursor;
		text->ptr[text->len] = '\0';
		editor->overflow = 0;
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if (key == 23 && editor->cursor) {
		size_t start = editor->cursor;
		int word = 0;
		while (start) {
			size_t previous =
				(size_t)(nb_utf8_prev(text->ptr, text->ptr + start) - text->ptr);
			unsigned cp = 0;
			size_t bytes = 0;
			if (!nb_utf8_read(text->ptr + previous, start - previous, &cp, &bytes))
				return NB_ERR_FORMAT;
			int space = nb_utf8_is_unicode_space_cp(cp);
			if (word && space)
				break;
			word |= !space;
			start = previous;
		}
		int rc = widget_editor_save(editor);
		if (rc)
			return rc;
		memmove(text->ptr + start, text->ptr + editor->cursor,
			text->len - editor->cursor + 1);
		text->len -= editor->cursor - start;
		editor->cursor = start;
		editor->overflow = 0;
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if ((key == 127 || key == 8) && editor->cursor) {
		size_t previous =
			(size_t)(nb_utf8_prev(text->ptr, text->ptr + editor->cursor) - text->ptr);
		int rc = widget_editor_save(editor);
		if (rc)
			return rc;
		memmove(text->ptr + previous, text->ptr + editor->cursor,
			text->len - editor->cursor + 1);
		text->len -= editor->cursor - previous;
		editor->cursor = previous;
		editor->overflow = 0;
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if ((key == MORPH_TUI_KEY_LEFT || key == 2) && editor->cursor) {
		editor->cursor =
			(size_t)(nb_utf8_prev(text->ptr, text->ptr + editor->cursor) - text->ptr);
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if ((key == MORPH_TUI_KEY_RIGHT || key == 6) && editor->cursor < text->len) {
		editor->cursor += nb_utf8_next_codepoint_len(text->ptr + editor->cursor,
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
		size_t bytes = nb_utf8_next_codepoint_len(text->ptr + editor->cursor,
							  text->len - editor->cursor);
		int rc = widget_editor_save(editor);
		if (rc)
			return rc;
		memmove(text->ptr + editor->cursor, text->ptr + editor->cursor + bytes,
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
	int rc = (nb_buf_reserve(text, 2) ? 0 : NB_ERR_NOMEM);

	if (!rc)
		rc = widget_editor_save(editor);
	if (rc != 0)
		return rc;
	memmove(text->ptr + editor->cursor + 1, text->ptr + editor->cursor,
		text->len - editor->cursor + 1);
	text->ptr[editor->cursor++] = (char)key;
	text->len++;
	return MORPH_TUI_WIDGET_CHANGED;
}

int morph_tui_widget_editor_state_view(struct morph_tui_widget_editor_state *editor, nb_buf *text,
				       size_t width, size_t *cursor_column)
{
	if (!editor || !text || !cursor_column || !editor->text.ptr)
		return NB_ERR_INVAL;
	*cursor_column = 0;
	if (!width)
		return 0;
	if (editor->secret) {
		const char *mask = editor->text.len ? "••••••••" : "";
		size_t visible = editor->text.len ? (width < 8 ? width : 8) : 0;

		*cursor_column = visible;
		return (nb_buf_append(text, mask, visible * strlen("•")) ? 0 : NB_ERR_NOMEM);
	}
	char *data = editor->text.ptr;
	char saved = data[editor->cursor];

	data[editor->cursor] = '\0';
	const char *start = morph_terminal_suffix_display_width_borrow(data, width - 1);

	*cursor_column = nb_utf8_width_cstr(start);
	data[editor->cursor] = saved;
	const char *end = morph_terminal_advance_display_width_borrow(start, width);

	return (nb_buf_append(text, start, (size_t)(end - start)) ? 0 : NB_ERR_NOMEM);
}

void morph_tui_widget_editor_state_cleanup(struct morph_tui_widget_editor_state *editor)
{
	if (!editor)
		return;
	if (editor->text.ptr)
		widget_editor_wipe(editor->text.ptr, editor->text.cap);
	nb_buf_cleanup(&editor->text);
	for (size_t i = 0; i < editor->undo.len; i++) {
		struct widget_editor_snapshot **slot = nb_vec_at(&editor->undo, i);
		widget_editor_snapshot_cleanup(slot);
	}
	nb_vec_cleanup(&editor->undo);
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
