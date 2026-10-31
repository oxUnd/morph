#ifndef MORPH_TUI_RENDERER_H
#define MORPH_TUI_RENDERER_H

#include <morph/tui/canvas.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_renderer;

/* Backends borrow the composed surface for present. The caller owns the
 * backend instance and keeps it alive while installed in a window. */
struct morph_tui_renderer_ops {
	int (*present)(struct morph_tui_renderer *, struct morph_tui_surface *, FILE *);
	void (*invalidate)(struct morph_tui_renderer *);
};
struct morph_tui_renderer {
	const struct morph_tui_renderer_ops *ops;
	void *data;
};

#ifdef __cplusplus
}
#endif
#endif
