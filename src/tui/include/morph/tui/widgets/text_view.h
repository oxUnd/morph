#ifndef MORPH_TUI_WIDGET_TEXT_VIEW_H
#define MORPH_TUI_WIDGET_TEXT_VIEW_H

#include "util/array.h"
#include <morph/tui/widget.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Text is borrowed until the next wrap/cleanup; lines contain byte spans. */
struct morph_tui_widget_text_line {
	size_t offset;
	size_t length;
};

struct morph_tui_widget_text_layout {
	const char *text;
	morph_array_t lines;
};

int morph_tui_widget_text_layout_wrap_borrow(struct morph_tui_widget_text_layout *view,
					     const char *text, size_t width);
void morph_tui_widget_text_layout_cleanup(struct morph_tui_widget_text_layout *view);

/* Returned nodes are controller-owned and borrowed until controller
 * destruction/build rollback. */
/* Retained, focusable document control. Text is copied and sanitized. */
struct morph_tui_view *
morph_tui_widget_text_view_add_borrow(struct morph_tui_builder *, struct morph_tui_view *parent,
				      const char *text,
				      const struct morph_tui_view_options *options);
/* Copies and sanitizes text; the caller retains the input. */
int morph_tui_widget_text_view_set_text_copy(struct morph_tui_view *, const char *text);
/* Copies all span text/styles. Terminal controls in text are sanitized. */
int morph_tui_widget_text_view_set_spans_copy(struct morph_tui_view *,
					    const struct morph_tui_text_span *, size_t count);
/* Content sizing measures all wrapped rows; default sizing is a scroll viewport. */
int morph_tui_widget_text_view_set_content_sizing(struct morph_tui_view *, int enabled);

#ifdef __cplusplus
}
#endif
#endif
