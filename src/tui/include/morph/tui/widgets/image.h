#ifndef MORPH_TUI_WIDGETS_IMAGE_H
#define MORPH_TUI_WIDGETS_IMAGE_H

#include <morph/tui/widget.h>
#include <morph/tui/media.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The widget retains its own asset reference. Size options use terminal cells;
 * an unspecified intrinsic height preserves aspect ratio with 8x16px cells. */
struct morph_tui_view *morph_tui_widget_image_add_borrow(struct morph_tui_builder *,
	struct morph_tui_view *parent, struct morph_tui_media_asset *,
	const struct morph_tui_view_options *);
int morph_tui_widget_image_set_asset_borrow(struct morph_tui_view *,
					   struct morph_tui_media_asset *);

#ifdef __cplusplus
}
#endif
#endif
