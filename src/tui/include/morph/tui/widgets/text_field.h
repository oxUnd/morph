#ifndef MORPH_TUI_WIDGET_TEXT_FIELD_H
#define MORPH_TUI_WIDGET_TEXT_FIELD_H

#include <morph/tui/widget.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Adds a controller-owned node, copying initial text. The node is borrowed
 * until controller destruction/build rollback. */
struct morph_tui_view *
morph_tui_widget_text_field_add_borrow(struct morph_tui_builder *, struct morph_tui_view *parent,
				       const char *text, size_t limit, int tag,
				       const struct morph_tui_view_options *options);
/* Borrowed text, invalidated by an edit/setter or controller destruction. */
const char *morph_tui_widget_text_field_text_borrow(const struct morph_tui_view *);
/* Copies text; the caller retains the input. */
int morph_tui_widget_text_field_set_text_copy(struct morph_tui_view *, const char *text);

#ifdef __cplusplus
}
#endif
#endif
