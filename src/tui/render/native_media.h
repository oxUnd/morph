#ifndef MORPH_TUI_RENDER_NATIVE_MEDIA_H
#define MORPH_TUI_RENDER_NATIVE_MEDIA_H

#include <morph/tui/frame.h>
#include <morph/tui/canvas.h>

struct render_native_media;

int render_native_media_prepare(struct render_native_media **, const struct morph_tui_surface *,
				struct morph_tui_frame *);
int render_native_media_present(struct render_native_media *, const struct morph_tui_surface *,
				FILE *output);
int render_native_media_append(struct render_native_media *, const struct morph_tui_surface *,
			       FILE *output, morph_buf_t *commands);
int render_native_media_needs_refresh(const struct render_native_media *,
	const struct morph_tui_surface *, const struct morph_tui_frame *, const char *rows);
void render_native_media_invalidate(struct render_native_media *);
void render_native_media_destroy(struct render_native_media *);

#endif
