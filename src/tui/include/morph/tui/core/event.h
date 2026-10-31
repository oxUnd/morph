#ifndef MORPH_TUI_CORE_EVENT_H
#define MORPH_TUI_CORE_EVENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum morph_tui_core_event_type {
	MORPH_TUI_CORE_EVENT_TICK,
	MORPH_TUI_CORE_EVENT_KEY,
	MORPH_TUI_CORE_EVENT_RESIZE,
	MORPH_TUI_CORE_EVENT_EOF,
	MORPH_TUI_CORE_EVENT_INPUT_READY,
	MORPH_TUI_CORE_EVENT_WAKE,
	MORPH_TUI_CORE_EVENT_MESSAGE,
	/* View controllers normalize printable UTF-8 and pasted text. Legacy
	 * controllers continue to receive their original byte key events. */
	MORPH_TUI_CORE_EVENT_TEXT,
	MORPH_TUI_CORE_EVENT_PASTE,
	MORPH_TUI_CORE_EVENT_SIGNAL
};

struct morph_tui_core_event {
	enum morph_tui_core_event_type type;
	int key;
	int pasting;
	int code;
	const void *data;
	const char *text;
	size_t text_length;
};

#ifdef __cplusplus
}
#endif
#endif
