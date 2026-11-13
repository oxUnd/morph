#ifndef MORPH_TUI_WIDGET_SCROLL_H
#define MORPH_TUI_WIDGET_SCROLL_H

#include <morph/tui/widget.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One content child is measured at viewport width and arranged at full height.
 * Scrolling clips descendants without changing their layout coordinates.
 * Follow starts enabled; manual scrolling suspends it until End reaches bottom. */
struct morph_tui_view *morph_tui_widget_scroll_add_borrow(struct morph_tui_builder *,
							  struct morph_tui_view *parent,
							  const struct morph_tui_view_options *);
int morph_tui_widget_scroll_handle_key(struct morph_tui_view *, int key);
int morph_tui_widget_scroll_follow(struct morph_tui_view *, int enabled);
int morph_tui_widget_scroll_offset(const struct morph_tui_view *);

#ifdef __cplusplus
}
#endif
#endif
