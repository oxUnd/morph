#ifndef MORPH_TUI_EVENT_H
#define MORPH_TUI_EVENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum morph_tui_event_type {
	MORPH_TUI_EVENT_TICK,
	MORPH_TUI_EVENT_KEY,
	MORPH_TUI_EVENT_RESIZE,
	MORPH_TUI_EVENT_EOF,
	MORPH_TUI_EVENT_INPUT_READY,
	MORPH_TUI_EVENT_WAKE,
	MORPH_TUI_EVENT_MESSAGE,
	/* View controllers normalize printable UTF-8 and pasted text. Legacy
	 * controllers continue to receive their original byte key events. */
	MORPH_TUI_EVENT_TEXT,
	MORPH_TUI_EVENT_PASTE,
	MORPH_TUI_EVENT_SIGNAL,
	MORPH_TUI_EVENT_MOUSE
};

enum morph_tui_mouse_button {
	MORPH_TUI_MOUSE_LEFT,
	MORPH_TUI_MOUSE_MIDDLE,
	MORPH_TUI_MOUSE_RIGHT
};

enum morph_tui_mouse_action {
	MORPH_TUI_MOUSE_PRESS,
	MORPH_TUI_MOUSE_RELEASE,
	MORPH_TUI_MOUSE_MOVE,
	/* Cancels a captured press on keyboard input, resize or page change. */
	MORPH_TUI_MOUSE_CANCEL
};

enum morph_tui_mouse_modifier {
	MORPH_TUI_MOUSE_SHIFT = 1,
	MORPH_TUI_MOUSE_ALT = 2,
	MORPH_TUI_MOUSE_CTRL = 4
};

/* Coordinates are zero-based window cells for controllers. Widget handlers
 * receive content-local coordinates after framework clipping/hit testing.
 * A handled left press captures moves/release; outside the effective ancestor
 * clip those events use (-1,-1). Unmatched releases never activate widgets. */
struct morph_tui_mouse_event {
	enum morph_tui_mouse_button button;
	enum morph_tui_mouse_action action;
	int x, y;
	unsigned modifiers;
};

struct morph_tui_event {
	enum morph_tui_event_type type;
	int key;
	int pasting;
	int code;
	const void *data;
	const char *text;
	size_t text_length;
	struct morph_tui_mouse_event mouse;
};

#ifdef __cplusplus
}
#endif
#endif
