#ifndef MORPH_TUI_RENDER_BACKEND_H
#define MORPH_TUI_RENDER_BACKEND_H

#include <morph/tui/render/canvas.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_render_backend;

/* Backends borrow the composed surface for present. The caller owns the
 * backend instance and keeps it alive while installed in a window. */
struct morph_tui_render_backend_ops {
	int (*present)(struct morph_tui_render_backend *, struct morph_tui_render_surface *,
		       FILE *);
	void (*invalidate)(struct morph_tui_render_backend *);
};
struct morph_tui_render_backend {
	const struct morph_tui_render_backend_ops *ops;
	void *data;
};

#ifdef __cplusplus
}
#endif
#endif
