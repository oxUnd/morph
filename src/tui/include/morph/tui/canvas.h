#ifndef MORPH_TUI_CANVAS_H
#define MORPH_TUI_CANVAS_H

#include "util/buf.h"
#include <morph/tui/media.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_surface;

enum morph_tui_style {
	MORPH_TUI_STYLE_NORMAL = 0,
	MORPH_TUI_STYLE_BOLD = 1,
	MORPH_TUI_STYLE_DIM = 2,
	MORPH_TUI_STYLE_SELECTED = 4,
	MORPH_TUI_STYLE_ITALIC = 8,
	MORPH_TUI_STYLE_UNDERLINE = 16,
	MORPH_TUI_STYLE_STRIKE = 32
};

/* Zero inherits the parent. DEFAULT explicitly resets to terminal defaults;
 * indexed values are 0..255 and RGB values are 0xRRGGBB. */
enum morph_tui_color_kind {
	MORPH_TUI_COLOR_INHERIT,
	MORPH_TUI_COLOR_DEFAULT,
	MORPH_TUI_COLOR_INDEXED,
	MORPH_TUI_COLOR_RGB
};

struct morph_tui_color {
	enum morph_tui_color_kind kind;
	unsigned value;
};

struct morph_tui_text_style {
	unsigned attributes;
	struct morph_tui_color foreground, background;
};

struct morph_tui_text_span {
	const char *text;
	struct morph_tui_text_style style;
	/* Optional URI, copied by TextView and Canvas. Terminal controls are rejected. */
	const char *hyperlink;
};

/* Canvas borrows surface until surface destruction. Coordinates are zero-based
 * and local. The clip is inherited from ancestors; only the renderer translates
 * coordinates to terminal cells. A canvas is a view, never an owner. */
struct morph_tui_canvas {
	struct morph_tui_surface *surface;
	int x, y, width, height;
	int left, top, right, bottom;
	struct morph_tui_text_style style;
};

/* out must be NULL; failure preserves it. Destroy releases the surface. */
int morph_tui_surface_create(struct morph_tui_surface **out);
void morph_tui_surface_destroy(struct morph_tui_surface *surface);
int morph_tui_surface_resize(struct morph_tui_surface *surface, int width, int height);
void morph_tui_surface_clear(struct morph_tui_surface *surface);
int morph_tui_surface_rows(struct morph_tui_surface *surface, morph_buf_t *rows);
/* Placements borrow immutable assets through the next clear/resize/destroy.
 * The surface retains assets independently of the drawing widget. */
size_t morph_tui_surface_image_count(const struct morph_tui_surface *);
const struct morph_tui_media_placement *morph_tui_surface_image_borrow(
	const struct morph_tui_surface *, size_t index);
/* Records a visible native image, respecting all ancestor/content clips. */
int morph_tui_canvas_image(struct morph_tui_canvas *, struct morph_tui_media_asset *,
			   int x, int y, int width, int height);
void morph_tui_canvas_root_borrow(struct morph_tui_surface *surface,
				  struct morph_tui_canvas *canvas);
void morph_tui_canvas_child_borrow(const struct morph_tui_canvas *parent, int x, int y, int width,
				   int height, struct morph_tui_canvas *child);
int morph_tui_canvas_text(struct morph_tui_canvas *canvas, int x, int y, const char *text,
			  unsigned style);
int morph_tui_canvas_fill(struct morph_tui_canvas *canvas, unsigned style);
int morph_tui_canvas_text_styled(struct morph_tui_canvas *, int x, int y, const char *,
				 const struct morph_tui_text_style *);
int morph_tui_canvas_text_link(struct morph_tui_canvas *, int x, int y, const char *,
	const struct morph_tui_text_style *, const char *uri);
int morph_tui_canvas_fill_styled(struct morph_tui_canvas *, const struct morph_tui_text_style *);
int morph_tui_text_style_equal(struct morph_tui_text_style, struct morph_tui_text_style);
int morph_tui_text_style_valid(struct morph_tui_text_style);
void morph_tui_canvas_cursor(struct morph_tui_canvas *canvas, int x, int y);
int morph_tui_surface_cursor(const struct morph_tui_surface *, int *x, int *y);
/* Inclusive visible-cell range, in either drag direction. Endpoints clamp to
 * the surface and expand to whole wide glyphs. Text excludes terminal styling
 * and links, trims trailing spaces per physical row, and separates rows with
 * newlines. Replaces text; a zero-sized surface produces an empty string. */
int morph_tui_surface_selection_text(struct morph_tui_surface *, int x1, int y1,
				     int x2, int y2, morph_buf_t *text);
/* Adds attributes to the same normalized range without changing cell text. */
int morph_tui_surface_selection_style(struct morph_tui_surface *, int x1, int y1,
				      int x2, int y2, unsigned attributes);

#ifdef __cplusplus
}
#endif
#endif
