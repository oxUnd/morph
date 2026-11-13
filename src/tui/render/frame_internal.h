#ifndef MORPH_TUI_RENDER_FRAME_INTERNAL_H
#define MORPH_TUI_RENDER_FRAME_INTERNAL_H

#include <morph/tui/frame.h>

/* Overlay commands are produced only by the framework's native media backend. */
int render_frame_present_composed(struct morph_tui_frame *, FILE *, int columns, int rows,
				  const char *lines, const morph_buf_t *overlay);

#endif
