#include <morph/tui/widgets/text_field.h>
#include <morph/tui/widgets/editor.h>
#include <morph/tui/widgets/list.h>
#include "util/error.h"

#include <string.h>

struct widget_text_field {
	struct morph_tui_widget_base base;
	struct morph_tui_widget_editor editor;
	morph_buf_t display;
};

static int widget_text_field_measure(struct morph_tui_core_view *view, int width,
				     struct morph_tui_core_view_size *size)
{
	(void)view;
	*size = (struct morph_tui_core_view_size){width, 1};
	return 0;
}

static int widget_text_field_draw(struct morph_tui_core_view *view,
				  struct morph_tui_render_canvas *canvas)
{
	struct widget_text_field *field = (struct widget_text_field *)view;
	size_t cursor;
	unsigned style = morph_tui_core_view_focused(view) ? MORPH_TUI_RENDER_STYLE_SELECTED : 0;
	int rc = morph_tui_render_canvas_fill(canvas, style);

	morph_buf_reset(&field->display);
	if (!rc)
		rc = morph_tui_widget_editor_view(&field->editor, &field->display,
						  canvas->width > 0 ? (size_t)canvas->width : 0,
						  &cursor);
	if (!rc)
		rc = morph_tui_render_canvas_text(canvas, 0, 0, morph_buf_cstr(&field->display),
						  style);
	if (!rc && morph_tui_core_view_focused(view))
		morph_tui_render_canvas_cursor(canvas, (int)cursor, 0);
	return rc;
}

static int widget_text_field_event(struct morph_tui_core_view *view,
				   const struct morph_tui_core_event *event)
{
	struct widget_text_field *field = (struct widget_text_field *)view;
	int result = 0;

	if (event->type == MORPH_TUI_CORE_EVENT_TEXT || event->type == MORPH_TUI_CORE_EVENT_PASTE) {
		/* A whole decoded text chunk fits or is rejected, preserving UTF-8. */
		if (event->text_length >= field->editor.limit - field->editor.text.len) {
			field->editor.overflow = 1;
			return 1;
		}
		for (size_t i = 0; i < event->text_length; i++) {
			result = morph_tui_widget_editor_update(&field->editor,
								(unsigned char)event->text[i]);
			if (result < 0)
				return result;
		}
		result = MORPH_TUI_WIDGET_CHANGED;
	} else if (event->type == MORPH_TUI_CORE_EVENT_KEY) {
		result = morph_tui_widget_editor_update(&field->editor, event->key);
	} else
		return 0;
	if (result < 0)
		return result;
	if (result != MORPH_TUI_WIDGET_CHANGED && result != MORPH_TUI_WIDGET_SUBMIT)
		return 0;
	morph_tui_core_view_invalidate(view, 0);
	int rc = morph_tui_widget_base_emit(view,
					    result == MORPH_TUI_WIDGET_SUBMIT
						    ? MORPH_TUI_WIDGET_ACTION_SUBMIT
						    : MORPH_TUI_WIDGET_ACTION_CHANGED,
					    0, morph_buf_cstr(&field->editor.text));

	return rc ? rc : 1;
}

static void widget_text_field_destroy(struct morph_tui_core_view *view)
{
	struct widget_text_field *field = (struct widget_text_field *)view;

	morph_tui_widget_editor_cleanup(&field->editor);
	morph_buf_cleanup(&field->display);
}

const char *morph_tui_widget_text_field_text(const struct morph_tui_core_view *view)
{
	if (!view || !view->ops || view->ops->draw != widget_text_field_draw)
		return "";
	return morph_buf_cstr(&((const struct widget_text_field *)view)->editor.text);
}

struct morph_tui_core_view *morph_tui_widget_text_field_create(
	struct morph_tui_core_builder *builder, struct morph_tui_core_view *parent,
	const char *text, size_t limit, int tag, const struct morph_tui_core_view_options *options)
{
	static const struct morph_tui_core_view_ops ops = {
		.measure = widget_text_field_measure,
		.draw = widget_text_field_draw,
		.event = widget_text_field_event,
		.destroy = widget_text_field_destroy,
	};
	struct widget_text_field *field = NULL;

	field = (struct widget_text_field *)morph_tui_widget_base_create(
		builder, parent, sizeof(*field), &ops, options);
	if (!field)
		return NULL;
	int rc = morph_tui_widget_editor_init(&field->editor, text, limit, 0);

	if (!rc)
		rc = morph_buf_init(&field->display, 64);
	morph_tui_core_builder_fail(builder, rc);
	morph_tui_core_view_set_focusable(&field->base.view, 1);
	morph_tui_widget_base_set_tag(&field->base.view, tag);
	return &field->base.view;
}
