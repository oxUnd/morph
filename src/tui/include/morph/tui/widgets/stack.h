#ifndef MORPH_TUI_WIDGET_STACK_H
#define MORPH_TUI_WIDGET_STACK_H

#include <morph/tui/widgets/base.h>

#ifdef __cplusplus
extern "C" {
#endif

enum morph_tui_widget_stack_axis {
	MORPH_TUI_WIDGET_STACK_VERTICAL,
	MORPH_TUI_WIDGET_STACK_HORIZONTAL
};

struct morph_tui_core_view *
morph_tui_widget_stack_create(struct morph_tui_core_builder *, struct morph_tui_core_view *parent,
			      enum morph_tui_widget_stack_axis axis, int spacing,
			      const struct morph_tui_core_view_options *options);

#ifdef __cplusplus
}
#endif
#endif
