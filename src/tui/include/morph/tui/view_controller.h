#ifndef MORPH_TUI_VIEW_CONTROLLER_H
#define MORPH_TUI_VIEW_CONTROLLER_H

#include <morph/tui/application.h>
#include <morph/tui/view.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_view_controller_state;
struct morph_tui_widget_action;

struct morph_tui_view_controller_ops {
	void (*build)(struct morph_tui_view_controller *, struct morph_tui_builder *);
	int (*action)(struct morph_tui_view_controller *, const struct morph_tui_widget_action *);
	int (*event)(struct morph_tui_view_controller *, const struct morph_tui_event *);
	int (*appear)(struct morph_tui_view_controller *);
	void (*disappear)(struct morph_tui_view_controller *);
	void (*destroy)(struct morph_tui_view_controller *, int result);
};

/* Embed first in custom controllers. Instances and their view scopes are
 * framework-owned. data is borrowed through destroy, including build failure.
 * destroy runs after view destruction; it must not access released views. */
struct morph_tui_view_controller {
	const struct morph_tui_view_controller_ops *ops;
	void *data;
	struct morph_tui_view_controller_state *state;
};

struct morph_tui_view_controller_spec {
	const struct morph_tui_view_controller_ops *ops;
	void *data;
	size_t size;
	/* Optional printable page shortcut, after the focused view declines it. */
	int quit_key;
	/* Optional business refresh timer. Zero disables periodic callbacks. */
	int tick_ms;
	/* Additional mouse/focus capabilities; alternate screen and paste are always on. */
	int terminal_flags;
};

/* Copies spec, borrowing its ops/data through the controller's destroy callback,
 * including failed build. Framework destruction never frees spec->data. */
int morph_tui_view_controller_present_borrow(struct morph_tui_application *app,
					     const struct morph_tui_view_controller_spec *spec);
int morph_tui_view_controller_dismiss(struct morph_tui_view_controller *controller, int result);
/* Borrows the owning application; never extends its lifetime. */
struct morph_tui_application *
morph_tui_view_controller_application_borrow(const struct morph_tui_view_controller *controller);

#ifdef __cplusplus
}
#endif
#endif
