#ifndef MORPH_TUI_LAYOUT_H
#define MORPH_TUI_LAYOUT_H

#include "util/buf.h"
#include <morph/tui/view.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-based origin and nonnegative extents. Empty regions are valid. */
struct morph_tui_terminal_rect {
	int x;
	int y;
	int width;
	int height;
};

/* Embed this base in custom layout managers. A view borrows its manager. */
struct morph_tui_layout_ops {
	int (*measure)(struct morph_tui_layout *, struct morph_tui_view *, int width,
		       struct morph_tui_view_size *);
	int (*arrange)(struct morph_tui_layout *, struct morph_tui_view *);
};
struct morph_tui_layout {
	const struct morph_tui_layout_ops *ops;
};

struct morph_tui_terminal_rect morph_tui_terminal_rect_inset(struct morph_tui_terminal_rect area,
							     int horizontal, int vertical);
struct morph_tui_terminal_rect
morph_tui_terminal_rect_take_top(struct morph_tui_terminal_rect *area, int height);
struct morph_tui_terminal_rect
morph_tui_terminal_rect_take_left(struct morph_tui_terminal_rect *area, int width);
int morph_tui_draw_region_cell(morph_buf_t *commands, struct morph_tui_terminal_rect area, int row,
			       int column, int width, const char *text, const char *style);

#ifdef __cplusplus
}
#endif
#endif
