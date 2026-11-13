#include <morph/tui/widgets/text_view.h>
#include <morph/tui/widgets/list.h>
#include "errors.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>
#include "nb/cleanup.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

struct widget_document_span {
	size_t start, length;
	struct morph_tui_text_style style;
	char *hyperlink;
};

static void widget_document_spans_cleanup(nb_vec *spans)
{
	struct widget_document_span *span;
	for (size_t _i_ = 0; _i_ < nb_vec_len(spans) &&
			     (span = (struct widget_document_span *)nb_vec_at(spans, _i_));
	     _i_++)
		free(span->hyperlink);
	nb_vec_cleanup(spans);
}

struct widget_document {
	struct morph_tui_widget base;
	struct morph_tui_widget_text_layout lines;
	char *text;
	nb_buf row;
	size_t offset, length;
	int width;
	int content_sizing;
	nb_vec spans;
};

static int widget_document_wrap(struct widget_document *document, int width)
{
	width = width > 0 ? width : 1;
	if (document->width == width)
		return 0;
	document->width = 0;
	int rc = morph_tui_widget_text_layout_wrap_borrow(
		&document->lines, document->text ? document->text : "", (size_t)width);
	if (!rc)
		document->width = width;
	return rc;
}

static int widget_document_measure(struct morph_tui_view *view, int width,
				   struct morph_tui_view_size *size)
{
	struct widget_document *document = (struct widget_document *)view;
	int rc = widget_document_wrap(document, width);
	if (rc)
		return rc;
	size_t rows = document->lines.lines.len;
	if (document->length && document->text[document->length - 1] == '\n')
		rows++;
	if (rows > INT_MAX)
		return NB_ERR_RANGE;
	*size = (struct morph_tui_view_size){width, document->content_sizing
		? (int)rows : 1};
	return 0;
}

static int widget_document_layout(struct morph_tui_view *view)
{
	struct widget_document *document = (struct widget_document *)view;
	struct morph_tui_view_rect content = morph_tui_view_content_rect(view);
	int rc = widget_document_wrap(document, content.width);
	int height = content.height > 0 ? content.height : 1;

	document->offset = morph_tui_widget_scroll_update(
		document->offset, document->lines.lines.len, (size_t)height, 0);
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
			nb_vec_at(&document->lines.lines, document->offset + (size_t)y);

		if (!line)
			break;
		int x = 0;
		for (size_t i = 0; i < document->spans.len; i++) {
			const struct widget_document_span *span = nb_vec_at(&document->spans, i);
			size_t start = span->start > line->offset ? span->start : line->offset;
			size_t end = span->start + span->length;
			if (end > line->offset + line->length)
				end = line->offset + line->length;
			if (end <= start)
				continue;
			nb_buf_clear(&document->row);
			int rc = (nb_buf_append(&document->row, document->text + start, end - start)
					  ? 0
					  : NB_ERR_NOMEM);
			struct morph_tui_text_style style = canvas->style;
			style.attributes |= span->style.attributes;
			if (span->style.foreground.kind)
				style.foreground = span->style.foreground;
			if (span->style.background.kind)
				style.background = span->style.background;
			if (!rc)
				rc = morph_tui_canvas_text_link(canvas, x, y,
								nb_buf_cstr(&document->row), &style,
								span->hyperlink);
			if (rc)
				return rc;
			x += (int)nb_utf8_width_cstr(nb_buf_cstr(&document->row));
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
		document->offset, document->lines.lines.len,
		content.height > 0 ? (size_t)content.height : 1, key);
	if (document->offset != old_offset)
		morph_tui_view_invalidate(view, 0);
	return 1;
}

static void widget_document_destroy(struct morph_tui_view *view)
{
	struct widget_document *document = (struct widget_document *)view;

	morph_tui_widget_text_layout_cleanup(&document->lines);
	nb_buf_cleanup(&document->row);
	widget_document_spans_cleanup(&document->spans);
	free(document->text);
}

int morph_tui_widget_text_view_set_text_copy(struct morph_tui_view *view, const char *text)
{
	if (!text)
		return NB_ERR_INVAL;
	struct morph_tui_text_span span = {.text = text};
	return morph_tui_widget_text_view_set_spans_copy(view, &span, 1);
}

int morph_tui_widget_text_view_set_spans_copy(struct morph_tui_view *view,
					    const struct morph_tui_text_span *spans, size_t count)
{
	if (!view || (!spans && count) || !view->ops || view->ops->draw != widget_document_draw)
		return NB_ERR_INVAL;
	if (!morph_tui_view_is_owner(view))
		return NB_ERR_NOT_PERMITTED;
	struct widget_document *document = (struct widget_document *)view;

	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_vec ranges NB_CLEANUP(widget_document_spans_cleanup) = {0};
	struct widget_document_span range;
	int rc = (nb_buf_init(&text), nb_buf_reserve(&text, 128) ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = (nb_vec_init(&ranges, sizeof(range)),
		      nb_vec_reserve(&ranges, count ? count : 1) ? 0 : NB_ERR_NOMEM);
	for (size_t i = 0; !rc && i < count; i++) {
		if (!spans[i].text || !morph_tui_text_style_valid(spans[i].style))
			return NB_ERR_INVAL;
		char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
			spans[i].text, strlen(spans[i].text), MORPH_TERMINAL_TEXT_MULTILINE, NULL);
		if (!safe)
			return NB_ERR_NOMEM;
		char *link NB_CLEANUP(nb_charp_cleanup) = NULL;
		if (spans[i].hyperlink) {
			link = morph_terminal_sanitize_dup(spans[i].hyperlink,
							   strlen(spans[i].hyperlink),
							   MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
			if (!link)
				return NB_ERR_NOMEM;
			if (strcmp(link, spans[i].hyperlink))
				return NB_ERR_INVAL;
		}
		range = (struct widget_document_span){text.len, strlen(safe), spans[i].style, NULL};
		rc = (nb_buf_append_cstr(&text, safe) ? 0 : NB_ERR_NOMEM);
		struct widget_document_span *slot = rc ? NULL : nb_vec_push(&ranges);
		if (!rc && !slot)
			rc = NB_ERR_NOMEM;
		if (!rc) {
			range.hyperlink = nb_charp_steal(&link);
			*slot = range;
		}
	}
	if (rc)
		return rc;
	int equal = document->text && !strcmp(document->text, nb_buf_cstr(&text)) &&
		    document->spans.len == ranges.len;
	for (size_t i = 0; equal && i < ranges.len; i++) {
		const struct widget_document_span *a = nb_vec_at(&document->spans, i);
		const struct widget_document_span *b = nb_vec_at(&ranges, i);
		equal = a->start == b->start && a->length == b->length &&
			morph_tui_text_style_equal(a->style, b->style) &&
			!strcmp(a->hyperlink ? a->hyperlink : "", b->hyperlink ? b->hyperlink : "");
	}
	if (equal)
		return 0;
	char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(nb_buf_cstr(&text));
	if (!copy)
		return NB_ERR_NOMEM;
	free(document->text);
	document->length = text.len;
	document->text = nb_charp_steal(&copy);
	widget_document_spans_cleanup(&document->spans);
	rc = (nb_vec_move(&document->spans, &ranges), 0);
	if (rc)
		return rc;
	document->width = 0;
	morph_tui_view_invalidate(view, 1);
	return 0;
}

int morph_tui_widget_text_view_set_content_sizing(struct morph_tui_view *view, int enabled)
{
	if (!view || !view->ops || view->ops->draw != widget_document_draw)
		return NB_ERR_INVAL;
	if (!morph_tui_view_is_owner(view))
		return NB_ERR_NOT_PERMITTED;
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

	morph_tui_builder_fail(builder,
			       (nb_buf_init(&document->row),
				nb_buf_reserve(&document->row, BUFSIZ) ? 0 : NB_ERR_NOMEM));
	morph_tui_builder_fail(builder, morph_tui_widget_text_view_set_text_copy(view, text));
	morph_tui_view_set_focusable(view, 1);
	return view;
}
