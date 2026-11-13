#ifndef MORPH_TUI_TERMINAL_H
#define MORPH_TUI_TERMINAL_H

#include <stdio.h>
#include <termios.h>

#ifdef __cplusplus
extern "C" {
#endif

enum morph_tui_terminal_session_flags {
	MORPH_TUI_TERMINAL_SESSION_ALTERNATE = 1,
	MORPH_TUI_TERMINAL_SESSION_PASTE = 2,
	MORPH_TUI_TERMINAL_SESSION_MOUSE = 4,
	MORPH_TUI_TERMINAL_SESSION_FOCUS = 8
};

/* Zero-initialize, enter once, then leave on every exit path. Screen ownership
 * must not overlap. A synchronous inline adapter must restore terminal state;
 * the Application reasserts its capabilities afterward. Signals belong to the caller. */
struct morph_tui_terminal_session {
	FILE *output;
	int input_fd;
	int flags;
	int active;
	struct termios saved;
};

int morph_tui_terminal_available(FILE *input, FILE *output);
void morph_tui_terminal_dimensions(int output_fd, int *columns, int *rows);
/* Move to a one-based cell and clear the remaining viewport. The caller reserves
 * the region and restores its retained frame after a synchronous inline prompt. */
int morph_tui_terminal_clear_below(FILE *output, int row, int column);
int morph_tui_terminal_session_enter(struct morph_tui_terminal_session *session, FILE *input,
				     FILE *output, int flags);
int morph_tui_terminal_session_leave(struct morph_tui_terminal_session *session);
int morph_tui_terminal_session_reassert(struct morph_tui_terminal_session *session);

#ifdef __cplusplus
}
#endif
#endif
