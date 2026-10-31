#ifndef SAMPLE_CUSTOMIZATION_WIDGET_COUNTER_H
#define SAMPLE_CUSTOMIZATION_WIDGET_COUNTER_H

#include <morph/tui.h>

struct morph_tui_view *sample_customization_widget_counter_create(struct morph_tui_builder *,
								  struct morph_tui_view *parent,
								  int tag, int value, int step);
int sample_customization_widget_counter_set(struct morph_tui_view *, int value, int step);
int sample_customization_widget_counter_value(const struct morph_tui_view *);
int sample_customization_widget_counter_step(const struct morph_tui_view *);

#endif
