#ifndef MORPH_TUI_WIDGET_CARD_H
#define MORPH_TUI_WIDGET_CARD_H

#include "util/buf.h"

#ifdef __cplusplus
extern "C" {
#endif

int morph_tui_widget_card_border(morph_buf_t *line, int width, int bottom);
int morph_tui_widget_card_row(morph_buf_t *line, int width, const char *text);

#ifdef __cplusplus
}
#endif
#endif
