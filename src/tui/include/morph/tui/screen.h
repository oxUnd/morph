#ifndef MORPH_TUI_SCREEN_H
#define MORPH_TUI_SCREEN_H

#include <morph/tui/event.h>
#include <morph/tui/input.h>
#include <morph/tui/layout.h>
#include <morph/tui/terminal.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standalone screen ownership: one terminal session and decoder. */
struct morph_tui_screen {
	struct morph_tui_terminal_session session;
	struct morph_tui_input input;
	struct morph_tui_terminal_rect area;
	const volatile sig_atomic_t *interrupted;
	int output_fd;
	int pasting;
	int dirty;
};

enum morph_tui_screen_result {
	MORPH_TUI_SCREEN_CONTINUE,
	MORPH_TUI_SCREEN_REDRAW,
	MORPH_TUI_SCREEN_CLOSE
};

struct morph_tui_screen_ops {
	int (*update)(void *data, const struct morph_tui_event *event);
	int (*draw)(void *data, const struct morph_tui_screen *screen);
};

int morph_tui_screen_open(struct morph_tui_screen *screen, FILE *input, FILE *output, int flags);
int morph_tui_screen_poll(struct morph_tui_screen *screen, struct morph_tui_event *event,
			  int timeout_ms);
int morph_tui_screen_run(struct morph_tui_screen *screen, const struct morph_tui_screen_ops *ops,
			 void *data);
int morph_tui_screen_close(struct morph_tui_screen *screen);

#ifdef __cplusplus
}
#endif
#endif
