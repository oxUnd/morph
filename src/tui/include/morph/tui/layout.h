#ifndef MORPH_TUI_LAYOUT_H
#define MORPH_TUI_LAYOUT_H

#include "util/buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One-based origin and nonnegative extents. Empty regions are valid. */
struct morph_tui_rect {
	int x;
	int y;
	int width;
	int height;
};

struct morph_tui_rect morph_tui_rect_inset(struct morph_tui_rect area,
					 int horizontal, int vertical);
struct morph_tui_rect morph_tui_rect_take_top(struct morph_tui_rect *area, int height);
struct morph_tui_rect morph_tui_rect_take_left(struct morph_tui_rect *area, int width);
int morph_tui_region_cell(morph_buf_t *commands, struct morph_tui_rect area,
		    int row, int column, int width, const char *text,
		    const char *style);

#ifdef __cplusplus
}
#endif
#endif
