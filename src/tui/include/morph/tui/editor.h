#ifndef MORPH_TUI_EDITOR_H
#define MORPH_TUI_EDITOR_H

#include <morph/tui/input.h>
#include "util/buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* UTF-8 byte input is accumulated without hand-written encoding logic.
 * limit includes the final NUL. Cleanup wipes owned input, including secrets. */
struct morph_tui_editor {
	morph_buf_t text;
	size_t cursor;
	size_t limit;
	int secret;
	int pasting;
	int overflow;
};

int morph_tui_editor_init(struct morph_tui_editor *editor, const char *text,
		    size_t limit, int secret);
int morph_tui_editor_update(struct morph_tui_editor *editor, int key);
int morph_tui_editor_valid(const struct morph_tui_editor *editor);
int morph_tui_editor_view(struct morph_tui_editor *editor, morph_buf_t *text,
		    size_t width, size_t *cursor_column);
void morph_tui_editor_cleanup(struct morph_tui_editor *editor);

#ifdef __cplusplus
}
#endif
#endif
