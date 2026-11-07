#ifndef MORPH_TUI_WIDGET_LABEL_H
#define MORPH_TUI_WIDGET_LABEL_H

#include <morph/tui/widget.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Controller-owned node, borrowed until controller destruction/build rollback.
 * Initial text and subsequent setter inputs are copied during the call. */
struct morph_tui_view *
morph_tui_widget_label_add_borrow(struct morph_tui_builder *, struct morph_tui_view *parent,
				  const char *text, const struct morph_tui_view_options *options);
int morph_tui_widget_label_set_text_copy(struct morph_tui_view *view, const char *text);

#ifdef __cplusplus
}
#endif
#endif
