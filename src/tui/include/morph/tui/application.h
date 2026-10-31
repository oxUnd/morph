#ifndef MORPH_TUI_APPLICATION_H
#define MORPH_TUI_APPLICATION_H

#include <morph/tui/components.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_app;

enum morph_tui_input_mode {
	/* The controller reads input, for example through a Readline adapter. */
	MORPH_TUI_INPUT_EXTERNAL,
	/* The application owns raw mode and delivers decoded key events. */
	MORPH_TUI_INPUT_KEYS
};

enum morph_tui_invalidation {
	MORPH_TUI_INVALIDATE_DISPLAY = 1,
	MORPH_TUI_INVALIDATE_LAYOUT = 2
};

/* Event handlers return zero if unhandled, one if handled, or an error.
 * Layout and drawing run after event dispatch and deferred transitions.
 * close runs once after removal, including shutdown and failed presentation;
 * it may release data. All callbacks execute on the creating thread. */
struct morph_tui_controller_ops {
	int (*event)(void *data, struct morph_tui_app *app,
		     const struct morph_tui_event *event);
	int (*layout)(void *data, const struct morph_tui_rect *area);
	int (*draw)(void *data, const struct morph_tui_screen *screen);
	int (*appear)(void *data, struct morph_tui_app *app);
	void (*disappear)(void *data, struct morph_tui_app *app);
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

struct morph_tui_app_options {
	FILE *input;
	FILE *output;
	/* Receives system/messages before the active controller. Returning one
	 * consumes the event. Input belongs exclusively to the active controller. */
	int (*event)(void *data, struct morph_tui_app *app,
		     const struct morph_tui_event *event);
	void *data;
};

/* The application borrows streams. No libuv types escape this interface.
 * Except wake/post, operations are owner-thread-only. Stop producers before
 * destroy; a successful post transfers payload ownership until release. */
int morph_tui_app_create(const struct morph_tui_app_options *options,
			 struct morph_tui_app **out);
void morph_tui_app_destroy(struct morph_tui_app *app);
int morph_tui_app_run(struct morph_tui_app *app);
int morph_tui_app_step(struct morph_tui_app *app, int wait);
int morph_tui_app_is_owner(const struct morph_tui_app *app);
void morph_tui_app_stop(struct morph_tui_app *app, int result);
int morph_tui_app_wake(struct morph_tui_app *app);
int morph_tui_app_post(struct morph_tui_app *app, int code, void *payload,
		       void (*release)(void *payload));

/* Present copies the descriptor and borrows ops until close. On failure the
 * caller retains data. Transitions commit outside event callbacks; dismiss
 * targets the controller that was active when requested. */
int morph_tui_app_present(struct morph_tui_app *app,
			 const struct morph_tui_controller *controller);
int morph_tui_app_dismiss(struct morph_tui_app *app, int result);
size_t morph_tui_app_depth(const struct morph_tui_app *app);
int morph_tui_app_transition_pending(const struct morph_tui_app *app);
void morph_tui_app_invalidate(struct morph_tui_app *app, unsigned flags);
/* Schedule one application tick. A negative delay cancels it. Controller
 * ticks and escape-sequence deadlines remain independent. */
void morph_tui_app_schedule(struct morph_tui_app *app, int delay_ms);
void morph_tui_app_set_input_enabled(struct morph_tui_app *app, int enabled);

#ifdef __cplusplus
}
#endif
#endif
