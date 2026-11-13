#include <morph/tui/widgets/text_field.h>
#include <morph/tui/widgets/editor.h>
#include <morph/tui/widgets/list.h>
#include "util/error.h"
#include "util/utf8.h"
#include "util/cleanup.h"
#include <morph/tui/widgets/text_view.h>
#include <limits.h>
#include <stdlib.h>

#include <string.h>

struct widget_text_field {
	struct morph_tui_widget base;
	struct morph_tui_widget_editor_state editor;
	morph_buf_t display;
	struct morph_tui_widget_text_layout lines;
	char *prompt;
	int multiline;
	int completion;
	int prompt_hanging_indent;
	struct morph_tui_text_style prompt_style;
	int (*decorate)(void *, const char *, size_t, struct morph_tui_text_style *);
	void *decoration_data;
	int (*resolve_atomic)(void *, const char *, size_t, int, size_t *, size_t *);
	void *atomic_data;
};

static size_t widget_text_field_indent(const struct widget_text_field *field, size_t row)
{
	return field->prompt && (field->prompt_hanging_indent || !row)
		       ? utf8_display_width(field->prompt)
		       : 0;
}

static size_t widget_text_field_capacity(const struct widget_text_field *field, int width,
					 size_t row)
{
	size_t indent = widget_text_field_indent(field, row);
	return width > 0 && (size_t)width > indent ? (size_t)width - indent : 1;
}

static int widget_text_field_wrap(struct widget_text_field *field, int width)
{
	struct morph_tui_widget_text_layout *layout = &field->lines;
	if (!layout->lines.elts) {
		struct morph_tui_widget_text_line line;
		int rc = morph_array_init(&layout->lines, 32, sizeof(line));
		if (rc)
			return rc;
	}
	morph_array_clear(&layout->lines);
	const char *text = morph_buf_cstr_borrow(&field->editor.text);
	layout->text = text;
	const char *start = text;
	const char *newline = strchr(start, '\n');
	while (*start) {
		size_t capacity = widget_text_field_capacity(field, width, layout->lines.nelts);
		const char *end = utf8_advance_display_width_borrow(start, capacity);
		if (newline && newline < end)
			end = newline;
		if (end == start && *start != '\n')
			end = utf8_advance_codepoints_borrow(start, 1);
		struct morph_tui_widget_text_line *line = morph_array_push_borrow(&layout->lines);
		if (!line)
			return -ENOMEM;
		*line = (struct morph_tui_widget_text_line){(size_t)(start - text),
							    (size_t)(end - start)};
		if (*end == '\n') {
			start = end + 1;
			newline = strchr(start, '\n');
		} else
			start = end;
	}
	int trailing = !field->editor.text.len || text[field->editor.text.len - 1] == '\n';
	if (!trailing && layout->lines.nelts) {
		size_t row = layout->lines.nelts - 1;
		const struct morph_tui_widget_text_line *last =
			morph_array_borrow(&layout->lines, row);
		trailing = utf8_display_width(text + last->offset) >=
			   widget_text_field_capacity(field, width, row);
	}
	if (trailing) {
		struct morph_tui_widget_text_line *line = morph_array_push_borrow(&layout->lines);
		if (!line)
			return -ENOMEM;
		*line = (struct morph_tui_widget_text_line){field->editor.text.len, 0};
	}
	return 0;
}

static int widget_text_field_measure(struct morph_tui_view *view, int width,
				     struct morph_tui_view_size *size)
{
	struct widget_text_field *field = (struct widget_text_field *)view;
	int rc = field->multiline ? widget_text_field_wrap(field, width) : 0;
	if (rc)
		return rc;
	size_t rows = field->multiline ? field->lines.lines.nelts : 1;
	if (rows > INT_MAX)
		return -EOVERFLOW;
	*size = (struct morph_tui_view_size){width, rows ? (int)rows : 1};
	return 0;
}

static int widget_text_field_draw_text(struct widget_text_field *field,
	struct morph_tui_canvas *canvas, int x, int y, const char *text, size_t offset,
	unsigned attributes)
{
	if (!field->decorate || field->editor.secret)
		return morph_tui_canvas_text(canvas, x, y, text, attributes);
	size_t length = strlen(text);
	for (size_t i = 0; i < length;) {
		unsigned cp = 0xfffdu;
		size_t bytes = 1;
		(void)utf8_decode_codepoint(text + i, length - i, &cp, &bytes);
		struct morph_tui_text_style style = canvas->style;
		style.attributes |= attributes;
		int rc = field->decorate(field->decoration_data,
			morph_buf_cstr_borrow(&field->editor.text), offset + i, &style);
		if (rc)
			return rc;
		if (!morph_tui_text_style_valid(style))
			return -EINVAL;
		char glyph[5] = {0};
		memcpy(glyph, text + i, bytes);
		rc = morph_tui_canvas_text_styled(canvas, x, y, glyph, &style);
		if (rc)
			return rc;
		int width = utf8_codepoint_width(cp);
		x += width < 0 ? 1 : width;
		i += bytes;
	}
	return 0;
}

static int widget_text_field_draw(struct morph_tui_view *view, struct morph_tui_canvas *canvas)
{
	struct widget_text_field *field = (struct widget_text_field *)view;
	if (field->multiline) {
		int rc = widget_text_field_wrap(field, canvas->width);
		if (rc)
			return rc;
		size_t cursor_row = 0, cursor_column = 0;
		for (size_t i = 0; i < field->lines.lines.nelts; i++) {
			const struct morph_tui_widget_text_line *line =
				morph_array_borrow(&field->lines.lines, i);
			if (field->editor.cursor >= line->offset) {
				cursor_row = i;
				size_t bytes = field->editor.cursor - line->offset;
				if (bytes > line->length)
					bytes = line->length;
				morph_buf_reset(&field->display);
				rc = morph_buf_append(&field->display,
						      field->editor.text.data + line->offset,
						      bytes);
				if (rc)
					return rc;
				cursor_column =
					utf8_display_width(morph_buf_cstr_borrow(&field->display));
			}
		}
		if (cursor_column >= widget_text_field_capacity(field, canvas->width, cursor_row)) {
			cursor_row++;
			cursor_column = 0;
		}
		size_t height = canvas->height > 0 ? (size_t)canvas->height : 1;
		size_t first = cursor_row >= height ? cursor_row - height + 1 : 0;
		for (size_t row = first;
		     row < field->lines.lines.nelts && row - first < height; row++) {
			const struct morph_tui_widget_text_line *line =
				morph_array_borrow(&field->lines.lines, row);
			morph_buf_reset(&field->display);
			rc = morph_buf_append(&field->display,
					      field->editor.text.data + line->offset, line->length);
			size_t indent = widget_text_field_indent(field, row);
			int prefix = indent < (size_t)canvas->width ? (int)indent : canvas->width;
			if (!rc)
				rc = widget_text_field_draw_text(field, canvas, prefix,
					(int)(row - first), morph_buf_cstr_borrow(&field->display),
					line->offset, 0);
			if (rc)
				return rc;
		}
		if (field->prompt && first == 0) {
			struct morph_tui_text_style style = field->prompt_style;
			if (!style.background.kind)
				style.background = canvas->style.background;
			rc = morph_tui_canvas_text_styled(canvas, 0, 0, field->prompt, &style);
			if (rc)
				return rc;
		}
		if (morph_tui_view_focused(view)) {
			size_t indent = widget_text_field_indent(field, cursor_row);
			int prefix = indent < (size_t)canvas->width ? (int)indent : canvas->width;
			morph_tui_canvas_cursor(canvas, prefix + (int)cursor_column,
						 (int)(cursor_row - first));
		}
		return 0;
	}
	size_t cursor;
	unsigned style = morph_tui_view_focused(view) ? MORPH_TUI_STYLE_SELECTED : 0;
	int rc = morph_tui_canvas_fill(canvas, style);

	morph_buf_reset(&field->display);
	if (!rc)
		rc = morph_tui_widget_editor_state_view(
			&field->editor, &field->display,
			canvas->width > 0 ? (size_t)canvas->width : 0, &cursor);
	if (!rc) {
		size_t offset = 0;
		if (field->decorate && !field->editor.secret && canvas->width > 0) {
			char *text = field->editor.text.data;
			char saved = text[field->editor.cursor];
			text[field->editor.cursor] = '\0';
			offset = (size_t)(utf8_suffix_display_width_borrow(text,
				(size_t)canvas->width - 1) - text);
			text[field->editor.cursor] = saved;
		}
		rc = widget_text_field_draw_text(field, canvas, 0, 0,
			morph_buf_cstr_borrow(&field->display), offset, style);
	}
	if (!rc && morph_tui_view_focused(view))
		morph_tui_canvas_cursor(canvas, (int)cursor, 0);
	return rc;
}

static int widget_text_field_atomic(struct widget_text_field *field, int key, int *handled)
{
	int backward = key == 127 || key == 8 || key == 23 || key == MORPH_TUI_KEY_LEFT ||
		       key == 2;
	int forward = key == MORPH_TUI_KEY_DELETE || key == 4 || key == MORPH_TUI_KEY_RIGHT ||
		      key == 6;
	*handled = 0;
	if (!field->resolve_atomic || field->editor.pasting || (!backward && !forward))
		return 0;
	size_t start = 0, end = 0;
	int rc = field->resolve_atomic(field->atomic_data,
		morph_buf_cstr_borrow(&field->editor.text), field->editor.cursor,
		backward, &start, &end);
	if (rc <= 0)
		return rc;
	if (start >= end || end > field->editor.text.len || field->editor.cursor < start ||
	    field->editor.cursor > end)
		return -EINVAL;
	int erase = key == 127 || key == 8 || key == 23 || key == MORPH_TUI_KEY_DELETE || key == 4;
	rc = erase ? morph_tui_widget_editor_state_replace_range_copy(&field->editor,
								     start, end, "")
		   : morph_tui_widget_editor_state_set_cursor(&field->editor,
							   backward ? start : end);
	if (rc)
		return rc;
	*handled = 1;
	return MORPH_TUI_WIDGET_CHANGED;
}

static int widget_text_field_event(struct morph_tui_view *view, const struct morph_tui_event *event)
{
	struct widget_text_field *field = (struct widget_text_field *)view;
	int result = 0;
	if (field->completion && event->type == MORPH_TUI_EVENT_KEY &&
	    !event->pasting && event->key == '\t') {
		int rc = morph_tui_widget_emit(view, MORPH_TUI_WIDGET_ACTION_COMPLETE,
			field->editor.cursor, morph_buf_cstr_borrow(&field->editor.text));
		return rc ? rc : 1;
	}

	if (event->type == MORPH_TUI_EVENT_TEXT || event->type == MORPH_TUI_EVENT_PASTE) {
		/* A whole decoded text chunk fits or is rejected, preserving UTF-8. */
		if (event->text_length >= field->editor.limit - field->editor.text.len) {
			field->editor.overflow = 1;
			return 1;
		}
		for (size_t i = 0; i < event->text_length; i++) {
			result = morph_tui_widget_editor_state_update(
				&field->editor, (unsigned char)event->text[i]);
			if (result < 0)
				return result;
		}
		result = MORPH_TUI_WIDGET_CHANGED;
	} else if (event->type == MORPH_TUI_EVENT_KEY) {
		int handled = 0;
		result = widget_text_field_atomic(field, event->key, &handled);
		if (result < 0)
			return result;
		if (!handled) {
			if (field->multiline && (event->key == '\n' ||
			    (event->pasting && event->key == '\r'))) {
				if (field->editor.text.len >= field->editor.limit - 1)
					return 1;
				int rc = morph_tui_widget_editor_state_replace_range_copy(
					&field->editor, field->editor.cursor, field->editor.cursor,
					"\n");
				if (rc)
					return rc;
				result = MORPH_TUI_WIDGET_CHANGED;
			} else
				result = morph_tui_widget_editor_state_update(&field->editor,
					event->key);
		}
	} else
		return 0;
	if (result < 0)
		return result;
	if (result != MORPH_TUI_WIDGET_CHANGED && result != MORPH_TUI_WIDGET_SUBMIT)
		return 0;
	morph_tui_view_invalidate(view, field->multiline);
	int rc = morph_tui_widget_emit(view,
				       result == MORPH_TUI_WIDGET_SUBMIT
					       ? MORPH_TUI_WIDGET_ACTION_SUBMIT
					       : MORPH_TUI_WIDGET_ACTION_CHANGED,
				       0, morph_buf_cstr_borrow(&field->editor.text));

	return rc ? rc : 1;
}

static void widget_text_field_destroy(struct morph_tui_view *view)
{
	struct widget_text_field *field = (struct widget_text_field *)view;

	morph_tui_widget_editor_state_cleanup(&field->editor);
	morph_buf_cleanup(&field->display);
	morph_tui_widget_text_layout_cleanup(&field->lines);
	free(field->prompt);
}

const char *morph_tui_widget_text_field_text_borrow(const struct morph_tui_view *view)
{
	if (!view || !view->ops || view->ops->draw != widget_text_field_draw)
		return "";
	return morph_buf_cstr_borrow(&((const struct widget_text_field *)view)->editor.text);
}

size_t morph_tui_widget_text_field_cursor(const struct morph_tui_view *view)
{
	if (!view || !view->ops || view->ops->draw != widget_text_field_draw)
		return 0;
	return ((const struct widget_text_field *)view)->editor.cursor;
}

int morph_tui_widget_text_field_set_cursor(struct morph_tui_view *view, size_t offset)
{
	if (!view || !view->ops || view->ops->draw != widget_text_field_draw)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	int rc = morph_tui_widget_editor_state_set_cursor(
		&((struct widget_text_field *)view)->editor, offset);
	if (!rc)
		morph_tui_view_invalidate(view, 0);
	return rc;
}

int morph_tui_widget_text_field_replace_range_copy(struct morph_tui_view *view, size_t start,
						 size_t end, const char *replacement)
{
	if (!view || !view->ops || view->ops->draw != widget_text_field_draw || !replacement)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct widget_text_field *field = (struct widget_text_field *)view;
	char *safe MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(replacement,
		strlen(replacement), field->multiline ? UTF8_TERMINAL_TEXT_MULTILINE :
		UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return -ENOMEM;
	int rc = morph_tui_widget_editor_state_replace_range_copy(&field->editor, start, end, safe);
	if (!rc)
		morph_tui_view_invalidate(view, field->multiline);
	return rc;
}

int morph_tui_widget_text_field_set_text_copy(struct morph_tui_view *view, const char *text)
{
	if (!view || !view->ops || view->ops->draw != widget_text_field_draw)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct widget_text_field *field = (struct widget_text_field *)view;
	struct morph_tui_widget_editor_state replacement
		MORPH_CLEANUP(morph_tui_widget_editor_state_cleanup) = {0};
	int rc = morph_tui_widget_editor_state_init(&replacement, text, field->editor.limit,
						    field->editor.secret);

	if (rc)
		return rc;
	morph_tui_widget_editor_state_move(&field->editor, &replacement);
	morph_tui_view_invalidate(view, field->multiline);
	return 0;
}

int morph_tui_widget_text_field_set_multiline(struct morph_tui_view *view, int enabled)
{
	if (!view || !view->ops || view->ops->draw != widget_text_field_draw)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct widget_text_field *field = (struct widget_text_field *)view;
	if (field->multiline == !!enabled)
		return 0;
	field->multiline = !!enabled;
	morph_tui_view_invalidate(view, 1);
	return 0;
}

int morph_tui_widget_text_field_set_completion(struct morph_tui_view *view, int enabled)
{
	if (!view || !view->ops || view->ops->draw != widget_text_field_draw)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	((struct widget_text_field *)view)->completion = !!enabled;
	return 0;
}

int morph_tui_widget_text_field_insert_text_copy(struct morph_tui_view *view, const char *text)
{
	if (!view || !view->ops || view->ops->draw != widget_text_field_draw || !text)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct widget_text_field *field = (struct widget_text_field *)view;
	return morph_tui_widget_text_field_replace_range_copy(view, field->editor.cursor,
		field->editor.cursor, text);
}

int morph_tui_widget_text_field_set_prompt_copy(struct morph_tui_view *view, const char *prompt)
{
	if (!view || !view->ops || view->ops->draw != widget_text_field_draw || !prompt)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	char *copy MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(
		prompt, strlen(prompt), UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!copy)
		return -ENOMEM;
	struct widget_text_field *field = (struct widget_text_field *)view;
	if (field->prompt && !strcmp(field->prompt, copy))
		return 0;
	free(field->prompt);
	field->prompt = morph_charp_steal(&copy);
	morph_tui_view_invalidate(view, 1);
	return 0;
}

int morph_tui_widget_text_field_set_prompt_hanging_indent(struct morph_tui_view *view,
							   int enabled)
{
	if (!view || !view->ops || view->ops->draw != widget_text_field_draw)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct widget_text_field *field = (struct widget_text_field *)view;
	if (field->prompt_hanging_indent != !!enabled) {
		field->prompt_hanging_indent = !!enabled;
		morph_tui_view_invalidate(view, 1);
	}
	return 0;
}

int morph_tui_widget_text_field_set_prompt_style(struct morph_tui_view *view,
	struct morph_tui_text_style style)
{
	if (!view || !view->ops || view->ops->draw != widget_text_field_draw ||
	    !morph_tui_text_style_valid(style))
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct widget_text_field *field = (struct widget_text_field *)view;
	if (!morph_tui_text_style_equal(field->prompt_style, style)) {
		field->prompt_style = style;
		morph_tui_view_invalidate(view, 0);
	}
	return 0;
}

int morph_tui_widget_text_field_set_decoration_borrow(struct morph_tui_view *view,
	int (*decorate)(void *, const char *, size_t, struct morph_tui_text_style *), void *data)
{
	if (!view || !view->ops || view->ops->draw != widget_text_field_draw)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct widget_text_field *field = (struct widget_text_field *)view;
	if (field->decorate != decorate || field->decoration_data != data) {
		field->decorate = decorate;
		field->decoration_data = data;
		morph_tui_view_invalidate(view, 0);
	}
	return 0;
}

int morph_tui_widget_text_field_set_atomic_range_resolver_borrow(struct morph_tui_view *view,
	int (*resolve)(void *, const char *, size_t, int, size_t *, size_t *), void *data)
{
	if (!view || !view->ops || view->ops->draw != widget_text_field_draw)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct widget_text_field *field = (struct widget_text_field *)view;
	field->resolve_atomic = resolve;
	field->atomic_data = data;
	return 0;
}

struct morph_tui_view *morph_tui_widget_text_field_add_borrow(
	struct morph_tui_builder *builder, struct morph_tui_view *parent, const char *text,
	size_t limit, int tag, const struct morph_tui_view_options *options)
{
	static const struct morph_tui_view_ops ops = {
		.measure = widget_text_field_measure,
		.draw = widget_text_field_draw,
		.event = widget_text_field_event,
		.destroy = widget_text_field_destroy,
	};
	struct widget_text_field *field = NULL;

	field = (struct widget_text_field *)morph_tui_widget_add_borrow(
		builder, parent, sizeof(*field), &ops, options);
	if (!field)
		return NULL;
	field->prompt_hanging_indent = 1;
	int rc = morph_tui_widget_editor_state_init(&field->editor, text, limit, 0);

	if (!rc)
		rc = morph_buf_init(&field->display, 64);
	morph_tui_builder_fail(builder, rc);
	morph_tui_view_set_focusable(&field->base.view, 1);
	morph_tui_widget_set_tag(&field->base.view, tag);
	return &field->base.view;
}
