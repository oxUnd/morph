#ifndef MORPH_TUI_CORE_APPLICATION_H
#define MORPH_TUI_CORE_APPLICATION_H

#include <morph/tui/core/controller.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_core_app;

enum morph_tui_core_invalidation {
	MORPH_TUI_CORE_INVALIDATE_DISPLAY = 1,
	MORPH_TUI_CORE_INVALIDATE_LAYOUT = 2
};

struct morph_tui_core_app_options {
	FILE *input;
	FILE *output;
	/* Receives system/messages before the active controller. Returning one
	 * consumes the event. Input belongs exclusively to the active controller. */
	int (*event)(void *data, struct morph_tui_core_app *app,
		     const struct morph_tui_core_event *event);
	void *data;
};

/* The application borrows streams. No libuv types escape this interface.
 * Except wake/post, operations are owner-thread-only. Stop producers before
 * destroy; a successful post transfers payload ownership until release. */
int morph_tui_core_app_create(const struct morph_tui_core_app_options *options,
			      struct morph_tui_core_app **out);
void morph_tui_core_app_destroy(struct morph_tui_core_app *app);
int morph_tui_core_app_run(struct morph_tui_core_app *app);
int morph_tui_core_app_step(struct morph_tui_core_app *app, int wait);
int morph_tui_core_app_is_owner(const struct morph_tui_core_app *app);
void morph_tui_core_app_stop(struct morph_tui_core_app *app, int result);
int morph_tui_core_app_wake(struct morph_tui_core_app *app);
int morph_tui_core_app_post(struct morph_tui_core_app *app, int code, void *payload,
			    void (*release)(void *payload));

/* Present copies the descriptor and borrows ops until close. On failure the
 * caller retains data. Transitions commit outside event callbacks; dismiss
 * targets the controller that was active when requested. */
int morph_tui_core_app_present(struct morph_tui_core_app *app,
			       const struct morph_tui_core_controller *controller);
int morph_tui_core_app_dismiss(struct morph_tui_core_app *app, int result);
size_t morph_tui_core_app_depth(const struct morph_tui_core_app *app);
int morph_tui_core_app_transition_pending(const struct morph_tui_core_app *app);
void morph_tui_core_app_invalidate(struct morph_tui_core_app *app, unsigned flags);
/* Schedule one application tick. A negative delay cancels it. Controller
 * ticks and escape-sequence deadlines remain independent. */
void morph_tui_core_app_schedule(struct morph_tui_core_app *app, int delay_ms);
void morph_tui_core_app_set_input_enabled(struct morph_tui_core_app *app, int enabled);

#ifdef __cplusplus
}
#endif
#endif
