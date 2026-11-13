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
	struct morph_tui_text_style prompt_style;
};

static int widget_text_field_measure(struct morph_tui_view *view, int width,
				     struct morph_tui_view_size *size)
{
	struct widget_text_field *field = (struct widget_text_field *)view;
	size_t prefix = field->prompt ? utf8_display_width(field->prompt) : 0;
	size_t available = width > 0 && (size_t)width > prefix ? (size_t)width - prefix : 1;
	int rc = 0;
	if (field->multiline)
		rc = morph_tui_widget_text_layout_wrap_borrow(&field->lines,
			morph_buf_cstr_borrow(&field->editor.text), available);
	if (rc)
		return rc;
	size_t rows = field->multiline ? field->lines.lines.nelts : 1;
	if (field->multiline && (!field->editor.text.len ||
	    field->editor.text.data[field->editor.text.len - 1] == '\n'))
		rows++;
	else if (field->multiline && field->lines.lines.nelts) {
		const struct morph_tui_widget_text_line *last = morph_array_borrow(
			&field->lines.lines, field->lines.lines.nelts - 1);
		if (utf8_display_width(field->editor.text.data + last->offset) == available)
			rows++;
	}
	if (rows > INT_MAX)
		return -EOVERFLOW;
	*size = (struct morph_tui_view_size){width, rows ? (int)rows : 1};
	return 0;
}

static int widget_text_field_draw(struct morph_tui_view *view, struct morph_tui_canvas *canvas)
{
	struct widget_text_field *field = (struct widget_text_field *)view;
	if (field->multiline) {
		int prefix = field->prompt ? (int)utf8_display_width(field->prompt) : 0;
		size_t width = canvas->width > prefix ? (size_t)(canvas->width - prefix) : 1;
		int rc = morph_tui_widget_text_layout_wrap_borrow(&field->lines,
			morph_buf_cstr_borrow(&field->editor.text), width);
		if (rc)
			return rc;
		size_t cursor_row = 0, cursor_column = 0;
		for (size_t i = 0; i < field->lines.lines.nelts; i++) {
			const struct morph_tui_widget_text_line *line = morph_array_borrow(&field->lines.lines, i);
			if (field->editor.cursor >= line->offset) {
				cursor_row = i;
				size_t bytes = field->editor.cursor - line->offset;
				if (bytes > line->length)
					bytes = line->length;
				morph_buf_reset(&field->display);
				rc = morph_buf_append(&field->display, field->editor.text.data + line->offset, bytes);
				if (rc)
					return rc;
				cursor_column = utf8_display_width(morph_buf_cstr_borrow(&field->display));
			}
		}
		if (field->editor.cursor == field->editor.text.len && field->editor.cursor &&
		    field->editor.text.data[field->editor.cursor - 1] == '\n') {
			cursor_row++;
			cursor_column = 0;
		}
		if (cursor_column >= width) {
			cursor_row++;
			cursor_column = 0;
		}
		size_t height = canvas->height > 0 ? (size_t)canvas->height : 1;
		size_t first = cursor_row >= height ? cursor_row - height + 1 : 0;
		for (size_t row = first; row < field->lines.lines.nelts && row - first < height; row++) {
			const struct morph_tui_widget_text_line *line = morph_array_borrow(&field->lines.lines, row);
			morph_buf_reset(&field->display);
			rc = morph_buf_append(&field->display, field->editor.text.data + line->offset, line->length);
			if (!rc)
				rc = morph_tui_canvas_text(canvas, prefix, (int)(row - first),
					morph_buf_cstr_borrow(&field->display), 0);
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
		if (morph_tui_view_focused(view))
			morph_tui_canvas_cursor(canvas, prefix + (int)cursor_column, (int)(cursor_row - first));
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
	if (!rc)
		rc = morph_tui_canvas_text(canvas, 0, 0, morph_buf_cstr_borrow(&field->display),
					   style);
	if (!rc && morph_tui_view_focused(view))
		morph_tui_canvas_cursor(canvas, (int)cursor, 0);
	return rc;
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
		if (field->multiline && (event->key == '\n' ||
		    (event->pasting && event->key == '\r'))) {
			if (field->editor.text.len >= field->editor.limit - 1)
				return 1;
			int rc = morph_buf_reserve(&field->editor.text, 2);
			if (rc)
				return rc;
			memmove(field->editor.text.data + field->editor.cursor + 1,
				field->editor.text.data + field->editor.cursor,
				field->editor.text.len - field->editor.cursor + 1);
			field->editor.text.data[field->editor.cursor++] = '\n';
			field->editor.text.len++;
			result = MORPH_TUI_WIDGET_CHANGED;
		} else
			result = morph_tui_widget_editor_state_update(&field->editor, event->key);
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

int morph_tui_widget_text_field_set_text_copy(struct morph_tui_view *view, const char *text)
{
	if (!view || !view->ops || view->ops->draw != widget_text_field_draw)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct widget_text_field *field = (struct widget_text_field *)view;
	struct morph_tui_widget_editor_state replacement = {0};
	int rc = morph_tui_widget_editor_state_init(&replacement, text, field->editor.limit,
						    field->editor.secret);

	if (rc)
		return rc;
	morph_tui_widget_editor_state_cleanup(&field->editor);
	field->editor = replacement;
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
	char *safe MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(text,
		strlen(text), field->multiline ? UTF8_TERMINAL_TEXT_MULTILINE :
		UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return -ENOMEM;
	size_t length = strlen(safe);
	if (length >= field->editor.limit - field->editor.text.len)
		return -E2BIG;
	int rc = morph_buf_reserve(&field->editor.text, length + 1);
	if (rc)
		return rc;
	memmove(field->editor.text.data + field->editor.cursor + length,
		field->editor.text.data + field->editor.cursor,
		field->editor.text.len - field->editor.cursor + 1);
	memcpy(field->editor.text.data + field->editor.cursor, safe, length);
	field->editor.cursor += length;
	field->editor.text.len += length;
	morph_tui_view_invalidate(view, field->multiline);
	return 0;
}

int morph_tui_widget_text_field_set_prompt_copy(struct morph_tui_view *view, const char *prompt)
{
	if (!view || !view->ops || view->ops->draw != widget_text_field_draw || !prompt)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	char *copy MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(prompt, strlen(prompt),
		UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
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
	int rc = morph_tui_widget_editor_state_init(&field->editor, text, limit, 0);

	if (!rc)
		rc = morph_buf_init(&field->display, 64);
	morph_tui_builder_fail(builder, rc);
	morph_tui_view_set_focusable(&field->base.view, 1);
	morph_tui_widget_set_tag(&field->base.view, tag);
	return &field->base.view;
}
