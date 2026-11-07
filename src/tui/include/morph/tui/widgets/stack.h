#ifndef MORPH_TUI_WIDGET_STACK_H
#define MORPH_TUI_WIDGET_STACK_H

#include <morph/tui/widget.h>

#ifdef __cplusplus
extern "C" {
#endif

enum morph_tui_widget_stack_axis {
	MORPH_TUI_WIDGET_STACK_VERTICAL,
	MORPH_TUI_WIDGET_STACK_HORIZONTAL
};

/* Controller-owned node, borrowed until controller destruction/build rollback. */
struct morph_tui_view *
morph_tui_widget_stack_add_borrow(struct morph_tui_builder *, struct morph_tui_view *parent,
				  enum morph_tui_widget_stack_axis axis, int spacing,
				  const struct morph_tui_view_options *options);

#ifdef __cplusplus
}
#endif
#endif
