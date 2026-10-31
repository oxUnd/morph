#ifndef MORPH_TUI_WIDGET_TEXT_FIELD_H
#define MORPH_TUI_WIDGET_TEXT_FIELD_H

#include <morph/tui/widgets/base.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_core_view *morph_tui_widget_text_field_create(
	struct morph_tui_core_builder *, struct morph_tui_core_view *parent, const char *text,
	size_t limit, int tag, const struct morph_tui_core_view_options *options);
const char *morph_tui_widget_text_field_text(const struct morph_tui_core_view *);

#ifdef __cplusplus
}
#endif
#endif
