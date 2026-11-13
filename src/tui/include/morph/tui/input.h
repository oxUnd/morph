#ifndef MORPH_TUI_INPUT_H
#define MORPH_TUI_INPUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes retain their values; zero means idle or an unsupported sequence.
 * Negative results are errors, including -ECANCELED on input EOF. */
enum morph_tui_key {
	MORPH_TUI_KEY_UP = 256,
	MORPH_TUI_KEY_DOWN,
	MORPH_TUI_KEY_LEFT,
	MORPH_TUI_KEY_RIGHT,
	MORPH_TUI_KEY_HOME,
	MORPH_TUI_KEY_END,
	MORPH_TUI_KEY_PAGE_UP,
	MORPH_TUI_KEY_PAGE_DOWN,
	MORPH_TUI_KEY_PASTE_BEGIN,
	MORPH_TUI_KEY_PASTE_END,
	MORPH_TUI_KEY_BACK_TAB,
	MORPH_TUI_KEY_WHEEL_UP,
	MORPH_TUI_KEY_WHEEL_DOWN,
	MORPH_TUI_KEY_FOCUS_IN,
	MORPH_TUI_KEY_FOCUS_OUT,
	MORPH_TUI_KEY_DELETE,
	MORPH_TUI_KEY_PASTE_CLIPBOARD
};

#define MORPH_TUI_INPUT_SEQUENCE_MAX 32

/* Zero-initialize per input stream. Decode and I/O are separate so a screen
 * can keep incomplete sequences across polls and interrupted system calls. */
struct morph_tui_input {
	char sequence[MORPH_TUI_INPUT_SEQUENCE_MAX];
	size_t length;
	int state;
	int64_t deadline_ms;
};

/* feed/expire perform no I/O or clock reads. read uses a monotonic deadline,
 * honors the caller's polling budget, and does not read ahead across screens. */
int morph_tui_input_feed(struct morph_tui_input *input, unsigned char byte);
int morph_tui_input_expire(struct morph_tui_input *input);
int morph_tui_input_read(struct morph_tui_input *input, int input_fd, int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
