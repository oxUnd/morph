#ifndef MORPH_TUI_TEXT_VIEW_H
#define MORPH_TUI_TEXT_VIEW_H

#include "util/array.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Text is borrowed until the next wrap/cleanup; lines contain byte spans. */
struct morph_tui_text_line {
	size_t offset;
	size_t length;
};

struct morph_tui_text_view {
	const char *text;
	morph_array_t lines;
};

int morph_tui_text_view_wrap(struct morph_tui_text_view *view, const char *text, size_t width);
void morph_tui_text_view_cleanup(struct morph_tui_text_view *view);

#ifdef __cplusplus
}
#endif
#endif
