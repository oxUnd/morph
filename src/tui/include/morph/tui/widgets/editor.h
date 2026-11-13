#ifndef MORPH_TUI_WIDGET_EDITOR_H
#define MORPH_TUI_WIDGET_EDITOR_H

#include <morph/tui/input.h>
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/vec.h"
#include "nb/cleanup.h"

#ifdef __cplusplus
extern "C" {
#endif

/* UTF-8 byte input is accumulated without hand-written encoding logic.
 * limit includes the final NUL. Cleanup wipes owned input, including secrets. */
struct morph_tui_widget_editor_state {
	nb_buf text;
	size_t cursor;
	size_t limit;
	int secret;
	int pasting;
	int overflow;
	/* Owned snapshots; secret fields never retain history. */
	nb_vec undo;
	size_t undo_bytes;
};

int morph_tui_widget_editor_state_init(struct morph_tui_widget_editor_state *editor,
				       const char *text, size_t limit, int secret);
int morph_tui_widget_editor_state_update(struct morph_tui_widget_editor_state *editor, int key);
int morph_tui_widget_editor_state_valid(const struct morph_tui_widget_editor_state *editor);
/* Cursor and range endpoints are UTF-8 byte boundaries. Range edits are one
 * undo step; rejected edits preserve text, cursor, and history. */
int morph_tui_widget_editor_state_set_cursor(struct morph_tui_widget_editor_state *, size_t offset);
int morph_tui_widget_editor_state_replace_range_copy(struct morph_tui_widget_editor_state *,
						     size_t start, size_t end,
						     const char *replacement);
/* Returns CHANGED on restoration, UNHANDLED without history, or an error. */
int morph_tui_widget_editor_state_undo(struct morph_tui_widget_editor_state *);
int morph_tui_widget_editor_state_view(struct morph_tui_widget_editor_state *editor, nb_buf *text,
				       size_t width, size_t *cursor_column);
void morph_tui_widget_editor_state_cleanup(struct morph_tui_widget_editor_state *editor);
/* Replaces destination and clears source without allocating. */
void morph_tui_widget_editor_state_move(struct morph_tui_widget_editor_state *destination,
					struct morph_tui_widget_editor_state *source);

#ifdef __cplusplus
}
#endif
#endif
