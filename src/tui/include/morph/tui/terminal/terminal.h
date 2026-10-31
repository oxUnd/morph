#ifndef MORPH_TUI_TERMINAL_H
#define MORPH_TUI_TERMINAL_H

#include <stdio.h>
#include <termios.h>

#ifdef __cplusplus
extern "C" {
#endif

enum morph_tui_terminal_session_flags {
	MORPH_TUI_TERMINAL_SESSION_ALTERNATE = 1,
	MORPH_TUI_TERMINAL_SESSION_PASTE = 2
};

/* Zero-initialize, enter once, then leave on every exit path. Sessions must
 * not overlap on the same terminal. Signal policy belongs to the caller. */
struct morph_tui_terminal_session {
	FILE *output;
	int input_fd;
	int flags;
	int active;
	struct termios saved;
};

int morph_tui_terminal_available(FILE *input, FILE *output);
void morph_tui_terminal_dimensions(int output_fd, int *columns, int *rows);
int morph_tui_terminal_session_enter(struct morph_tui_terminal_session *session, FILE *input,
				     FILE *output, int flags);
int morph_tui_terminal_session_leave(struct morph_tui_terminal_session *session);

#ifdef __cplusplus
}
#endif
#endif
