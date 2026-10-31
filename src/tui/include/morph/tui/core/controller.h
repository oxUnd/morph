#ifndef MORPH_TUI_CORE_CONTROLLER_H
#define MORPH_TUI_CORE_CONTROLLER_H

#include <morph/tui/core/screen.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_core_app;

enum morph_tui_core_input_mode {
	/* The controller reads input, for example through a Readline adapter. */
	MORPH_TUI_CORE_INPUT_EXTERNAL,
	/* The application owns raw mode and delivers decoded key events. */
	MORPH_TUI_CORE_INPUT_KEYS
};

/* Event handlers return zero if unhandled, one if handled, or an error.
 * Layout and drawing run after event dispatch and deferred transitions.
 * close runs once after removal, including shutdown and failed presentation;
 * it may release data. All callbacks execute on the creating thread. */
struct morph_tui_core_controller_ops {
	int (*event)(void *data, struct morph_tui_core_app *app,
		     const struct morph_tui_core_event *event);
	int (*layout)(void *data, const struct morph_tui_core_layout_rect *area);
	int (*draw)(void *data, const struct morph_tui_core_screen *screen);
	int (*appear)(void *data, struct morph_tui_core_app *app);
	void (*disappear)(void *data, struct morph_tui_core_app *app);
	void (*close)(void *data, int result);
};

struct morph_tui_core_controller {
	const struct morph_tui_core_controller_ops *ops;
	void *data;
	enum morph_tui_core_input_mode input_mode;
	int terminal_flags;
	/* Zero disables periodic ticks; decoded screens also probe for resize. */
	int tick_ms;
};

#ifdef __cplusplus
}
#endif
#endif
