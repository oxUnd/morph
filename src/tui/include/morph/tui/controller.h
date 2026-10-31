#ifndef MORPH_TUI_CONTROLLER_H
#define MORPH_TUI_CONTROLLER_H

#include <morph/tui/screen.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_application;

enum morph_tui_input_mode {
	/* The controller reads input, for example through a Readline adapter. */
	MORPH_TUI_INPUT_EXTERNAL,
	/* The application owns raw mode and delivers decoded key events. */
	MORPH_TUI_INPUT_KEYS
};

/* Event handlers return zero if unhandled, one if handled, or an error.
 * Layout and drawing run after event dispatch and deferred transitions.
 * close runs once after removal, including shutdown and failed presentation;
 * it may release data. All callbacks execute on the creating thread. */
struct morph_tui_controller_ops {
	int (*event)(void *data, struct morph_tui_application *app,
		     const struct morph_tui_event *event);
	int (*layout)(void *data, const struct morph_tui_terminal_rect *area);
	int (*draw)(void *data, const struct morph_tui_screen *screen);
	int (*appear)(void *data, struct morph_tui_application *app);
	void (*disappear)(void *data, struct morph_tui_application *app);
	void (*close)(void *data, int result);
};

struct morph_tui_controller {
	const struct morph_tui_controller_ops *ops;
	void *data;
	enum morph_tui_input_mode input_mode;
	int terminal_flags;
	/* Zero disables periodic ticks; decoded screens also probe for resize. */
	int tick_ms;
};

#ifdef __cplusplus
}
#endif
#endif
