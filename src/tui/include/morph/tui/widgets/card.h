#ifndef MORPH_TUI_WIDGET_CARD_H
#define MORPH_TUI_WIDGET_CARD_H

#include "nb/buf.h"
#include "nb/cleanup.h"

#ifdef __cplusplus
extern "C" {
#endif

int morph_tui_widget_card_border(nb_buf *line, int width, int bottom);
int morph_tui_widget_card_row(nb_buf *line, int width, const char *text);

#ifdef __cplusplus
}
#endif
#endif
