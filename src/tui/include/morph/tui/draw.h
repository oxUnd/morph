#ifndef MORPH_TUI_DRAW_H
#define MORPH_TUI_DRAW_H

#include "util/buf.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize commands with morph_buf_init before composing.
 * Compose without terminal I/O. A failed append marks the buffer as failed,
 * so morph_tui_draw_write cannot emit a partially constructed screen. */
int morph_tui_draw_write(FILE *output, const morph_buf_t *commands);
int morph_tui_draw_cell_append_aligned(morph_buf_t *commands, int columns, int rows, int row,
				       int column, int width, const char *text, const char *style,
				       int right);
int morph_tui_draw_cell_append(morph_buf_t *commands, int columns, int rows, int row, int column,
			       int width, const char *text, const char *style);

/* One-based coordinates. Text is untrusted; style is a trusted ANSI string.
 * Clip to the viewport, sanitize controls, and pad to the requested width. */
int morph_tui_draw_cell(FILE *output, int columns, int rows, int row, int column, int width,
			const char *text, const char *style);

#ifdef __cplusplus
}
#endif
#endif
