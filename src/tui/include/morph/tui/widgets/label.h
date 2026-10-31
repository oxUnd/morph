#ifndef MORPH_TUI_WIDGET_LABEL_H
#define MORPH_TUI_WIDGET_LABEL_H

#include <morph/tui/widgets/base.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_core_view *
morph_tui_widget_label_create(struct morph_tui_core_builder *, struct morph_tui_core_view *parent,
			      const char *text, const struct morph_tui_core_view_options *options);
int morph_tui_widget_label_set_text(struct morph_tui_core_view *view, const char *text);

#ifdef __cplusplus
}
#endif
#endif
