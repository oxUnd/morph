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
enum tui_key {
	TUI_KEY_UP = 256,
	TUI_KEY_DOWN,
	TUI_KEY_LEFT,
	TUI_KEY_RIGHT,
	TUI_KEY_HOME,
	TUI_KEY_END,
	TUI_KEY_PAGE_UP,
	TUI_KEY_PAGE_DOWN,
	TUI_KEY_PASTE_BEGIN,
	TUI_KEY_PASTE_END
};

enum tui_session_flags {
	TUI_SESSION_ALTERNATE = 1,
	TUI_SESSION_PASTE = 2
};

/* Zero-initialize, enter once, then leave on every exit path. Sessions must
 * not overlap on the same terminal. Signal policy belongs to the caller. */
struct tui_session {
	FILE *output;
	int input_fd;
	int flags;
	int active;
	struct termios saved;
};

int tui_available(FILE *input, FILE *output);
void tui_dimensions(int output_fd, int *columns, int *rows);
int tui_session_enter(struct tui_session *session, FILE *input, FILE *output,
		      int flags);
int tui_session_leave(struct tui_session *session);
#define TUI_INPUT_SEQUENCE_MAX 32

/* Zero-initialize per input stream. Decode and I/O are separate so a screen
 * can keep incomplete sequences across polls and interrupted system calls. */
struct tui_input {
	char sequence[TUI_INPUT_SEQUENCE_MAX];
	size_t length;
	int state;
	int64_t deadline_ms;
};

/* feed/expire perform no I/O or clock reads. read uses a monotonic deadline,
 * honors the caller's polling budget, and does not read ahead across screens. */
int tui_input_feed(struct tui_input *input, unsigned char byte);
int tui_input_expire(struct tui_input *input);
int tui_input_read(struct tui_input *input, int input_fd, int timeout_ms);

/* Cache for a full-screen frame. Zero-initialize and cleanup after use.
 * Rows are newline-separated trusted terminal text, already sanitized and
 * clipped by the caller; ANSI styles are allowed, cursor controls are not. */
struct tui_frame {
	morph_buf_t previous;
	morph_buf_t output;
	FILE *target;
	int columns;
	int rows;
};

int tui_frame_present(struct tui_frame *frame, FILE *output,
		      int columns, int rows, const char *lines);
void tui_frame_invalidate(struct tui_frame *frame);
void tui_frame_cleanup(struct tui_frame *frame);

/* Initialize commands with morph_buf_init before composing.
 * Compose without terminal I/O. A failed append marks the buffer as failed,
 * so tui_output_write cannot emit a partially constructed screen. */
int tui_output_write(FILE *output, const morph_buf_t *commands);
int tui_cell_append_aligned(morph_buf_t *commands, int columns, int rows,
			    int row, int column, int width, const char *text,
			    const char *style, int right);
int tui_cell_append(morph_buf_t *commands, int columns, int rows,
		    int row, int column, int width, const char *text,
		    const char *style);

/* One-based coordinates. Text is untrusted; style is a trusted ANSI string.
 * Clip to the viewport, sanitize controls, and pad to the requested width. */
int tui_cell(FILE *output, int columns, int rows, int row, int column,
	     int width, const char *text, const char *style);

#ifdef __cplusplus
}
#endif
#endif
