#ifndef MORPH_TUI_APPLICATION_H
#define MORPH_TUI_APPLICATION_H

#include <morph/tui/controller.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_application;
struct morph_tui_application_state;
struct morph_tui_view_controller_spec;
struct morph_tui_window;

struct morph_tui_application_ops {
	int (*launch)(struct morph_tui_application *);
	int (*event)(struct morph_tui_application *, const struct morph_tui_event *);
	void (*terminate)(struct morph_tui_application *, int result);
	/* The stopped application is still initialized. Join producers here. */
	void (*prepare_termination)(struct morph_tui_application *, int result);
};

/* Embed first in a custom application. The caller owns an initialized instance
 * and its method table. state is private; zero-initialize before init/main. */
struct morph_tui_application {
	const struct morph_tui_application_ops *ops;
	struct morph_tui_application_state *state;
};

struct morph_tui_application_options {
	FILE *input, *output;
	/* Embedded hosts retain their existing process signal policy. */
	int external_signals;
	/* Optional host system-event adapter, before controller dispatch. */
	int (*event)(void *, struct morph_tui_application *, const struct morph_tui_event *);
	void *data;
};

enum morph_tui_invalidation { MORPH_TUI_INVALIDATE_DISPLAY = 1, MORPH_TUI_INVALIDATE_LAYOUT = 2 };

/* The payload and its release callback are one transferable owner. Without a
 * release callback, data is borrowed and must outlive message delivery. */
struct morph_tui_message {
	void *data;
	void (*release)(void *);
};

void morph_tui_message_cleanup(struct morph_tui_message *message);

/* init/deinit work on caller-owned instances, including subclasses.
 * create/destroy additionally allocate/free the default Application instance.
 * Except post/wake, operations belong to the creating thread. Teardown inside
 * callbacks is refused; request stop and let the owner deinitialize afterward. */
int morph_tui_application_init(struct morph_tui_application *,
			       const struct morph_tui_application_options *options);
void morph_tui_application_deinit(struct morph_tui_application *);
/* out must be NULL; failure preserves it. */
int morph_tui_application_create(const struct morph_tui_application_options *,
				 struct morph_tui_application **out);
void morph_tui_application_destroy(struct morph_tui_application *);
/* launch runs once during init, before the caller builds its root page.
 * run/step retain the same initialized state until deinit. */
int morph_tui_application_run(struct morph_tui_application *);
int morph_tui_application_step(struct morph_tui_application *, int wait);
int morph_tui_application_is_owner(const struct morph_tui_application *);
void morph_tui_application_stop(struct morph_tui_application *, int result);
int morph_tui_application_wake(struct morph_tui_application *);
/* Signal without a payload. */
int morph_tui_application_post(struct morph_tui_application *, int code);
/* Success clears message before publication; failure leaves it unchanged.
 * Join producers before deinit. UI/model messages must not borrow dead views. */
int morph_tui_application_post_take(struct morph_tui_application *, int code,
				    struct morph_tui_message *message);
struct morph_tui_window *morph_tui_application_window_borrow(struct morph_tui_application *);
size_t morph_tui_application_depth(const struct morph_tui_application *);
int morph_tui_application_transition_pending(const struct morph_tui_application *);
void morph_tui_application_invalidate(struct morph_tui_application *, unsigned flags);
void morph_tui_application_schedule(struct morph_tui_application *, int delay_ms);
void morph_tui_application_set_input_enabled(struct morph_tui_application *, int enabled);
/* Takes the descriptor and its close obligation; success clears it, failure
 * leaves it unchanged. ops is borrowed through close; data must survive close.
 * The close callback decides whether data is owned or borrowed. */
int morph_tui_application_present_controller_take(struct morph_tui_application *,
						  struct morph_tui_controller *);
int morph_tui_application_dismiss_controller(struct morph_tui_application *, int result);
/* Convenience entry point: initialize, present root, run, deinitialize, report
 * errors after terminal restoration, and return a process exit status. */
int morph_tui_application_main(struct morph_tui_application *,
			       const struct morph_tui_view_controller_spec *root);

#ifdef __cplusplus
}
#endif
#endif
