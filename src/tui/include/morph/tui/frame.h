#ifndef MORPH_TUI_FRAME_H
#define MORPH_TUI_FRAME_H

#include "nb/buf.h"
#include "nb/cleanup.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cache for a full-screen frame. Zero-initialize and cleanup after use.
 * Rows are newline-separated trusted terminal text, already sanitized and
 * clipped by the caller; ANSI styles are allowed, cursor controls are not. */
struct morph_tui_frame {
	nb_buf previous;
	nb_buf output;
	FILE *target;
	int columns;
	int rows;
};

int morph_tui_frame_present(struct morph_tui_frame *frame, FILE *output, int columns, int rows,
			    const char *lines);
void morph_tui_frame_invalidate(struct morph_tui_frame *frame);
void morph_tui_frame_cleanup(struct morph_tui_frame *frame);

#ifdef __cplusplus
}
#endif
#endif
