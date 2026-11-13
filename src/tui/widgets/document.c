#include <morph/tui/widgets/text_view.h>
#include <morph/tui/widgets/list.h>
#include "util/error.h"
#include "util/utf8.h"
#include "util/cleanup.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

struct widget_document_span {
	size_t start, length;
	struct morph_tui_text_style style;
};

struct widget_document {
	struct morph_tui_widget base;
	struct morph_tui_widget_text_layout lines;
	char *text;
	morph_buf_t row;
	size_t offset;
	int width;
	int content_sizing;
	morph_array_t spans;
};

static int widget_document_measure(struct morph_tui_view *view, int width,
				   struct morph_tui_view_size *size)
{
	struct widget_document *document = (struct widget_document *)view;
	int rc = morph_tui_widget_text_layout_wrap_borrow(
		&document->lines, document->text ? document->text : "", width > 0 ? (size_t)width : 1);
	if (rc)
		return rc;
	size_t rows = document->lines.lines.nelts;
	size_t length = document->text ? strlen(document->text) : 0;
	if (length && document->text[length - 1] == '\n')
		rows++;
	if (rows > INT_MAX)
		return -EOVERFLOW;
	document->width = width;
	*size = (struct morph_tui_view_size){width, document->content_sizing
		? (int)rows : 1};
	return 0;
}

static int widget_document_layout(struct morph_tui_view *view)
{
	struct widget_document *document = (struct widget_document *)view;
	struct morph_tui_view_rect content = morph_tui_view_content_rect(view);
	int width = content.width > 0 ? content.width : 1;
	int rc = 0;

	if (document->width != width) {
		rc = morph_tui_widget_text_layout_wrap_borrow(
			&document->lines, document->text ? document->text : "", (size_t)width);
		if (!rc)
			document->width = width;
	}
	int height = content.height > 0 ? content.height : 1;

	document->offset = morph_tui_widget_scroll_update(
		document->offset, document->lines.lines.nelts, (size_t)height, 0);
	return rc;
}

static int widget_document_draw(struct morph_tui_view *view, struct morph_tui_canvas *canvas)
{
	struct widget_document *document = (struct widget_document *)view;

	int first = canvas->top > canvas->y ? canvas->top - canvas->y : 0;
	int end_row = canvas->bottom - canvas->y;
	if (end_row > canvas->height)
		end_row = canvas->height;
	for (int y = first; y < end_row; y++) {
		struct morph_tui_widget_text_line *line =
			morph_array_borrow(&document->lines.lines, document->offset + (size_t)y);

		if (!line)
			break;
		int x = 0;
		for (size_t i = 0; i < document->spans.nelts; i++) {
			const struct widget_document_span *span = morph_array_borrow(&document->spans, i);
			size_t start = span->start > line->offset ? span->start : line->offset;
			size_t end = span->start + span->length;
			if (end > line->offset + line->length)
				end = line->offset + line->length;
			if (end <= start)
				continue;
			morph_buf_reset(&document->row);
			int rc = morph_buf_append(&document->row, document->text + start, end - start);
			struct morph_tui_text_style style = canvas->style;
			style.attributes |= span->style.attributes;
			if (span->style.foreground.kind)
				style.foreground = span->style.foreground;
			if (span->style.background.kind)
				style.background = span->style.background;
			if (!rc)
				rc = morph_tui_canvas_text_styled(canvas, x, y,
					morph_buf_cstr_borrow(&document->row), &style);
			if (rc)
				return rc;
			x += (int)utf8_display_width(morph_buf_cstr_borrow(&document->row));
		}
	}
	return 0;
}

static int widget_document_event(struct morph_tui_view *view, const struct morph_tui_event *event)
{
	if (event->pasting || event->type != MORPH_TUI_EVENT_KEY)
		return 0;
	int key = event->key;

	if (key != MORPH_TUI_KEY_UP && key != MORPH_TUI_KEY_DOWN && key != MORPH_TUI_KEY_PAGE_UP &&
	    key != MORPH_TUI_KEY_PAGE_DOWN && key != MORPH_TUI_KEY_HOME && key != MORPH_TUI_KEY_END)
		return 0;
	struct widget_document *document = (struct widget_document *)view;
	struct morph_tui_view_rect content = morph_tui_view_content_rect(view);
	size_t old_offset = document->offset;

	document->offset = morph_tui_widget_scroll_update(
		document->offset, document->lines.lines.nelts,
		content.height > 0 ? (size_t)content.height : 1, key);
	if (document->offset != old_offset)
		morph_tui_view_invalidate(view, 0);
	return 1;
}

static void widget_document_destroy(struct morph_tui_view *view)
{
	struct widget_document *document = (struct widget_document *)view;

	morph_tui_widget_text_layout_cleanup(&document->lines);
	morph_buf_cleanup(&document->row);
	morph_array_cleanup(&document->spans);
	free(document->text);
}

int morph_tui_widget_text_view_set_text_copy(struct morph_tui_view *view, const char *text)
{
	if (!text)
		return -EINVAL;
	struct morph_tui_text_span span = {.text = text};
	return morph_tui_widget_text_view_set_spans_copy(view, &span, 1);
}

int morph_tui_widget_text_view_set_spans_copy(struct morph_tui_view *view,
					    const struct morph_tui_text_span *spans, size_t count)
{
	if (!view || (!spans && count) || !view->ops || view->ops->draw != widget_document_draw)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct widget_document *document = (struct widget_document *)view;

	morph_buf_t text MORPH_CLEANUP(morph_buf_cleanup) = {0};
	morph_array_t ranges MORPH_CLEANUP(morph_array_cleanup) = {0};
	struct widget_document_span range;
	int rc = morph_buf_init(&text, 128);
	if (!rc)
		rc = morph_array_init(&ranges, count ? count : 1, sizeof(range));
	for (size_t i = 0; !rc && i < count; i++) {
		if (!spans[i].text || !morph_tui_text_style_valid(spans[i].style))
			return -EINVAL;
		char *safe MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(
			spans[i].text, strlen(spans[i].text), UTF8_TERMINAL_TEXT_MULTILINE, NULL);
		if (!safe)
			return -ENOMEM;
		range = (struct widget_document_span){text.len, strlen(safe), spans[i].style};
		rc = morph_buf_puts(&text, safe);
		struct widget_document_span *slot = rc ? NULL : morph_array_push_borrow(&ranges);
		if (!rc && !slot)
			rc = -ENOMEM;
		if (!rc)
			*slot = range;
	}
	if (rc)
		return rc;
	int equal = document->text && !strcmp(document->text, morph_buf_cstr_borrow(&text)) &&
		document->spans.nelts == ranges.nelts;
	for (size_t i = 0; equal && i < ranges.nelts; i++) {
		const struct widget_document_span *a = morph_array_borrow(&document->spans, i);
		const struct widget_document_span *b = morph_array_borrow(&ranges, i);
		equal = a->start == b->start && a->length == b->length &&
			morph_tui_text_style_equal(a->style, b->style);
	}
	if (equal)
		return 0;
	free(document->text);
	document->text = morph_buf_detach(&text);
	morph_array_cleanup(&document->spans);
	rc = morph_array_take(&document->spans, &ranges);
	if (rc)
		return rc;
	document->width = 0;
	morph_tui_view_invalidate(view, 1);
	return 0;
}

int morph_tui_widget_text_view_set_content_sizing(struct morph_tui_view *view, int enabled)
{
	if (!view || !view->ops || view->ops->draw != widget_document_draw)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct widget_document *document = (struct widget_document *)view;
	if (document->content_sizing != !!enabled) {
		document->content_sizing = !!enabled;
		morph_tui_view_invalidate(view, 1);
	}
	return 0;
}

struct morph_tui_view *
morph_tui_widget_text_view_add_borrow(struct morph_tui_builder *builder,
				      struct morph_tui_view *parent, const char *text,
				      const struct morph_tui_view_options *options)
{
	static const struct morph_tui_view_ops ops = {
		.measure = widget_document_measure,
		.layout = widget_document_layout,
		.draw = widget_document_draw,
		.event = widget_document_event,
		.destroy = widget_document_destroy,
	};
	struct widget_document *document = NULL;

	document = (struct widget_document *)morph_tui_widget_add_borrow(
		builder, parent, sizeof(*document), &ops, options);
	if (!document)
		return NULL;
	struct morph_tui_view *view = &document->base.view;

	morph_tui_builder_fail(builder, morph_buf_init(&document->row, BUFSIZ));
	morph_tui_builder_fail(builder, morph_tui_widget_text_view_set_text_copy(view, text));
	morph_tui_view_set_focusable(view, 1);
	return view;
}
