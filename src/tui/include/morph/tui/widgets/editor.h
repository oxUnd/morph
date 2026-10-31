#ifndef MORPH_TUI_WIDGET_EDITOR_H
#define MORPH_TUI_WIDGET_EDITOR_H

#include <morph/tui/input.h>
#include "util/buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* UTF-8 byte input is accumulated without hand-written encoding logic.
 * limit includes the final NUL. Cleanup wipes owned input, including secrets. */
struct morph_tui_widget_editor_state {
	morph_buf_t text;
	size_t cursor;
	size_t limit;
	int secret;
	int pasting;
	int overflow;
};

int morph_tui_widget_editor_state_init(struct morph_tui_widget_editor_state *editor,
				       const char *text, size_t limit, int secret);
int morph_tui_widget_editor_state_update(struct morph_tui_widget_editor_state *editor, int key);
int morph_tui_widget_editor_state_valid(const struct morph_tui_widget_editor_state *editor);
int morph_tui_widget_editor_state_view(struct morph_tui_widget_editor_state *editor,
				       morph_buf_t *text, size_t width, size_t *cursor_column);
void morph_tui_widget_editor_state_cleanup(struct morph_tui_widget_editor_state *editor);

#ifdef __cplusplus
}
#endif
#endif
