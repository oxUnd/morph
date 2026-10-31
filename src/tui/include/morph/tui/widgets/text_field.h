#ifndef MORPH_TUI_WIDGET_TEXT_FIELD_H
#define MORPH_TUI_WIDGET_TEXT_FIELD_H

#include <morph/tui/widget.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_view *
morph_tui_widget_text_field_create(struct morph_tui_builder *, struct morph_tui_view *parent,
				   const char *text, size_t limit, int tag,
				   const struct morph_tui_view_options *options);
const char *morph_tui_widget_text_field_text(const struct morph_tui_view *);
int morph_tui_widget_text_field_set_text(struct morph_tui_view *, const char *text);

#ifdef __cplusplus
}
#endif
#endif
