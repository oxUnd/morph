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

int morph_tui_widget_text_layout_wrap(struct morph_tui_widget_text_layout *view, const char *text,
				      size_t width);
void morph_tui_widget_text_layout_cleanup(struct morph_tui_widget_text_layout *view);

/* Retained, focusable document control. Text is copied and sanitized. */
struct morph_tui_view *
morph_tui_widget_text_view_create(struct morph_tui_builder *, struct morph_tui_view *parent,
				  const char *text, const struct morph_tui_view_options *options);
int morph_tui_widget_text_view_set_text(struct morph_tui_view *, const char *text);

#ifdef __cplusplus
}
#endif
#endif
