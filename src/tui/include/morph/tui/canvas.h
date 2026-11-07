#ifndef MORPH_TUI_CANVAS_H
#define MORPH_TUI_CANVAS_H

#include "util/buf.h"

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_surface;

enum morph_tui_style {
	MORPH_TUI_STYLE_NORMAL = 0,
	MORPH_TUI_STYLE_BOLD = 1,
	MORPH_TUI_STYLE_DIM = 2,
	MORPH_TUI_STYLE_SELECTED = 4
};

/* Canvas borrows surface until surface destruction. Coordinates are zero-based
 * and local. The clip is inherited from ancestors; only the renderer translates
 * coordinates to terminal cells. A canvas is a view, never an owner. */
struct morph_tui_canvas {
	struct morph_tui_surface *surface;
	int x, y, width, height;
	int left, top, right, bottom;
};

/* out must be NULL; failure preserves it. Destroy releases the surface. */
int morph_tui_surface_create(struct morph_tui_surface **out);
void morph_tui_surface_destroy(struct morph_tui_surface *surface);
int morph_tui_surface_resize(struct morph_tui_surface *surface, int width, int height);
void morph_tui_surface_clear(struct morph_tui_surface *surface);
int morph_tui_surface_rows(struct morph_tui_surface *surface, morph_buf_t *rows);
void morph_tui_canvas_root_borrow(struct morph_tui_surface *surface,
				  struct morph_tui_canvas *canvas);
void morph_tui_canvas_child_borrow(const struct morph_tui_canvas *parent, int x, int y, int width,
				   int height, struct morph_tui_canvas *child);
int morph_tui_canvas_text(struct morph_tui_canvas *canvas, int x, int y, const char *text,
			  unsigned style);
int morph_tui_canvas_fill(struct morph_tui_canvas *canvas, unsigned style);
void morph_tui_canvas_cursor(struct morph_tui_canvas *canvas, int x, int y);
int morph_tui_surface_cursor(const struct morph_tui_surface *, int *x, int *y);

#ifdef __cplusplus
}
#endif
#endif
