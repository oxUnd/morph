#include <morph/tui/widgets/text_view.h>
#include <morph/tui/widgets/list.h>
#include "util/error.h"
#include "util/utf8.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct widget_document {
	struct morph_tui_widget_base base;
	struct morph_tui_widget_text_view lines;
	char *text;
	morph_buf_t row;
	size_t offset;
	int width;
};

static int widget_document_measure(struct morph_tui_core_view *view, int width,
				   struct morph_tui_core_view_size *size)
{
	(void)view;
	*size = (struct morph_tui_core_view_size){width, 1};
	return 0;
}

static int widget_document_layout(struct morph_tui_core_view *view)
{
	struct widget_document *document = (struct widget_document *)view;
	struct morph_tui_core_view_rect frame = morph_tui_core_view_frame(view);
	struct morph_tui_core_view_options opts = morph_tui_core_view_get_options(view);
	int width = frame.width > opts.padding * 2 ? frame.width - opts.padding * 2 : 1;
	int rc = 0;

	if (document->width != width) {
		rc = morph_tui_widget_text_view_wrap(
			&document->lines, document->text ? document->text : "", (size_t)width);
		if (!rc)
			document->width = width;
	}
	int height = frame.height > opts.padding * 2 ? frame.height - opts.padding * 2 : 1;

	document->offset = morph_tui_widget_scroll_update(
		document->offset, document->lines.lines.nelts, (size_t)height, 0);
	return rc;
}

static int widget_document_draw(struct morph_tui_core_view *view,
				struct morph_tui_render_canvas *canvas)
{
	struct widget_document *document = (struct widget_document *)view;

	for (int y = 0; y < canvas->height; y++) {
		struct morph_tui_widget_text_line *line =
			morph_array_get(&document->lines.lines, document->offset + (size_t)y);

		if (!line)
			break;
		morph_buf_reset(&document->row);
		int rc = morph_buf_append(&document->row, document->text + line->offset,
					  line->length);

		if (!rc)
			rc = morph_tui_render_canvas_text(canvas, 0, y,
							  morph_buf_cstr(&document->row), 0);
		if (rc)
			return rc;
	}
	return 0;
}

static int widget_document_event(struct morph_tui_core_view *view,
				 const struct morph_tui_core_event *event)
{
	if (event->pasting || event->type != MORPH_TUI_CORE_EVENT_KEY)
		return 0;
	int key = event->key;

	if (key != MORPH_TUI_TERMINAL_KEY_UP && key != MORPH_TUI_TERMINAL_KEY_DOWN &&
	    key != MORPH_TUI_TERMINAL_KEY_PAGE_UP && key != MORPH_TUI_TERMINAL_KEY_PAGE_DOWN &&
	    key != MORPH_TUI_TERMINAL_KEY_HOME && key != MORPH_TUI_TERMINAL_KEY_END)
		return 0;
	struct widget_document *document = (struct widget_document *)view;
	struct morph_tui_core_view_rect frame = morph_tui_core_view_frame(view);

	document->offset =
		morph_tui_widget_scroll_update(document->offset, document->lines.lines.nelts,
					       frame.height > 0 ? (size_t)frame.height : 1, key);
	morph_tui_core_view_invalidate(view, 0);
	return 1;
}

static void widget_document_destroy(struct morph_tui_core_view *view)
{
	struct widget_document *document = (struct widget_document *)view;

	morph_tui_widget_text_view_cleanup(&document->lines);
	morph_buf_cleanup(&document->row);
	free(document->text);
}

int morph_tui_widget_text_view_set_text(struct morph_tui_core_view *view, const char *text)
{
	if (!view || !text || !view->ops || view->ops->draw != widget_document_draw)
		return -EINVAL;
	if (!morph_tui_core_view_is_owner(view))
		return -EPERM;
	struct widget_document *document = (struct widget_document *)view;
	char *copy =
		utf8_terminal_sanitize_dup(text, strlen(text), UTF8_TERMINAL_TEXT_MULTILINE, NULL);

	if (!copy)
		return -ENOMEM;
	free(document->text);
	document->text = copy;
	document->width = 0;
	morph_tui_core_view_invalidate(view, 1);
	return 0;
}

struct morph_tui_core_view *
morph_tui_widget_text_view_create(struct morph_tui_core_builder *builder,
				  struct morph_tui_core_view *parent, const char *text,
				  const struct morph_tui_core_view_options *options)
{
	static const struct morph_tui_core_view_ops ops = {
		.measure = widget_document_measure,
		.layout = widget_document_layout,
		.draw = widget_document_draw,
		.event = widget_document_event,
		.destroy = widget_document_destroy,
	};
	struct widget_document *document = NULL;

	document = (struct widget_document *)morph_tui_widget_base_create(
		builder, parent, sizeof(*document), &ops, options);
	if (!document)
		return NULL;
	struct morph_tui_core_view *view = &document->base.view;

	morph_tui_core_builder_fail(builder, morph_buf_init(&document->row, BUFSIZ));
	morph_tui_core_builder_fail(builder, morph_tui_widget_text_view_set_text(view, text));
	morph_tui_core_view_set_focusable(view, 1);
	return view;
}
