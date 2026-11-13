#ifndef MORPH_TUI_MEDIA_H
#define MORPH_TUI_MEDIA_H

#include "nb/buf.h"
#include "nb/cleanup.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_media_asset;

/* Uses terminal capability environment variables without emitting probes. */
int morph_tui_media_protocol_available(void);

/* Immutable decoded RGBA assets. Outputs must be NULL; failure preserves them.
 * PNG input is bounded before decoding. RGBA input contains width*height*4 bytes. */
int morph_tui_media_asset_create_png_copy(struct morph_tui_media_asset **out,
					 const unsigned char *bytes, size_t length);
int morph_tui_media_asset_create_rgba_copy(struct morph_tui_media_asset **out,
					  const unsigned char *pixels, int width, int height);
/* Each retained reference is an independent owner released with destroy. */
struct morph_tui_media_asset *morph_tui_media_asset_retain(struct morph_tui_media_asset *);
void morph_tui_media_asset_destroy(struct morph_tui_media_asset *);
int morph_tui_media_asset_width(const struct morph_tui_media_asset *);
int morph_tui_media_asset_height(const struct morph_tui_media_asset *);
const unsigned char *morph_tui_media_asset_pixels_borrow(const struct morph_tui_media_asset *);
/* Stable content key for renderer caches, including dimensions and RGBA bytes. */
uint64_t morph_tui_media_asset_key(const struct morph_tui_media_asset *);
/* Appends an encoded PNG of a bounded pixel crop. out remains caller-owned. */
int morph_tui_media_asset_png_append(const struct morph_tui_media_asset *, int x, int y, int width,
				     int height, nb_buf *out);

struct morph_tui_media_placement {
	struct morph_tui_media_asset *asset;
	/* Clipped destination cells in surface coordinates. */
	int x, y, width, height;
	/* Corresponding source pixel crop. */
	int source_x, source_y, source_width, source_height;
};

#ifdef __cplusplus
}
#endif
#endif
