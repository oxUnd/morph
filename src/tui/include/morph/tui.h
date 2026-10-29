#ifndef MORPH_TUI_H
#define MORPH_TUI_H

#include "util/buf.h"
#include <stdio.h>
#include <termios.h>

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
	MORPH_TUI_KEY_PASTE_END
};

enum morph_tui_session_flags {
	MORPH_TUI_SESSION_ALTERNATE = 1,
	MORPH_TUI_SESSION_PASTE = 2
};

/* Zero-initialize, enter once, then leave on every exit path. Sessions must
 * not overlap on the same terminal. Signal policy belongs to the caller. */
struct morph_tui_session {
	FILE *output;
	int input_fd;
	int flags;
	int active;
	struct termios saved;
};

int morph_tui_available(FILE *input, FILE *output);
void morph_tui_dimensions(int output_fd, int *columns, int *rows);
int morph_tui_session_enter(struct morph_tui_session *session, FILE *input, FILE *output,
		      int flags);
int morph_tui_session_leave(struct morph_tui_session *session);
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

/* Cache for a full-screen frame. Zero-initialize and cleanup after use.
 * Rows are newline-separated trusted terminal text, already sanitized and
 * clipped by the caller; ANSI styles are allowed, cursor controls are not. */
struct morph_tui_frame {
	morph_buf_t previous;
	morph_buf_t output;
	FILE *target;
	int columns;
	int rows;
};

int morph_tui_frame_present(struct morph_tui_frame *frame, FILE *output,
		      int columns, int rows, const char *lines);
void morph_tui_frame_invalidate(struct morph_tui_frame *frame);
void morph_tui_frame_cleanup(struct morph_tui_frame *frame);

/* Initialize commands with morph_buf_init before composing.
 * Compose without terminal I/O. A failed append marks the buffer as failed,
 * so morph_tui_output_write cannot emit a partially constructed screen. */
int morph_tui_output_write(FILE *output, const morph_buf_t *commands);
int morph_tui_cell_append_aligned(morph_buf_t *commands, int columns, int rows,
			    int row, int column, int width, const char *text,
			    const char *style, int right);
int morph_tui_cell_append(morph_buf_t *commands, int columns, int rows,
		    int row, int column, int width, const char *text,
		    const char *style);

/* One-based coordinates. Text is untrusted; style is a trusted ANSI string.
 * Clip to the viewport, sanitize controls, and pad to the requested width. */
int morph_tui_cell(FILE *output, int columns, int rows, int row, int column,
	     int width, const char *text, const char *style);

#ifdef __cplusplus
}
#endif
#endif
