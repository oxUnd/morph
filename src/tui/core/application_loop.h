#ifndef MORPH_TUI_CORE_APPLICATION_LOOP_H
#define MORPH_TUI_CORE_APPLICATION_LOOP_H

#include <morph/tui/application.h>

#ifdef __cplusplus
extern "C" {
#endif

struct core_application_loop;

struct core_application_loop_options {
	struct morph_tui_application *application;
	FILE *input;
	FILE *output;
	/* Receives system/messages before the active controller. Returning one
	 * consumes the event. Input belongs exclusively to the active controller. */
	int (*event)(void *data, struct morph_tui_application *app,
		     const struct morph_tui_event *event);
	void *data;
};

/* The application borrows streams. No libuv types escape this interface.
 * Except wake/post, operations are owner-thread-only. Stop producers before
 * destroy; a successful post transfers payload ownership until release. */
struct morph_tui_window *core_application_loop_window(struct core_application_loop *app);
int core_application_loop_create(const struct core_application_loop_options *options,
				 struct core_application_loop **out);
int core_application_loop_busy(const struct core_application_loop *app);
int core_application_loop_result(const struct core_application_loop *app);
void core_application_loop_destroy(struct core_application_loop *app);
int core_application_loop_run(struct core_application_loop *app);
int core_application_loop_step(struct core_application_loop *app, int wait);
int core_application_loop_is_owner(const struct core_application_loop *app);
void core_application_loop_stop(struct core_application_loop *app, int result);
int core_application_loop_wake(struct core_application_loop *app);
int core_application_loop_post(struct core_application_loop *app, int code, void *payload,
			       void (*release)(void *payload));

/* Present copies the descriptor and borrows ops until close. On failure the
 * caller retains data. Transitions commit outside event callbacks; dismiss
 * targets the controller that was active when requested. */
int core_application_loop_present(struct core_application_loop *app,
				  const struct morph_tui_controller *controller);
int core_application_loop_dismiss(struct core_application_loop *app, int result);
size_t core_application_loop_depth(const struct core_application_loop *app);
int core_application_loop_transition_pending(const struct core_application_loop *app);
void core_application_loop_invalidate(struct core_application_loop *app, unsigned flags);
/* Schedule one application tick. A negative delay cancels it. Controller
 * ticks and escape-sequence deadlines remain independent. */
void core_application_loop_schedule(struct core_application_loop *app, int delay_ms);
void core_application_loop_set_input_enabled(struct core_application_loop *app, int enabled);

#ifdef __cplusplus
}
#endif
#endif
