#ifndef MORPH_TUI_CORE_VIEW_CONTROLLER_H
#define MORPH_TUI_CORE_VIEW_CONTROLLER_H

#include <morph/tui/core/application.h>
#include <morph/tui/core/view.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_core_view_controller_state;
struct morph_tui_widget_action;

struct morph_tui_core_view_controller_ops {
	void (*build)(struct morph_tui_core_view_controller *, struct morph_tui_core_builder *);
	int (*action)(struct morph_tui_core_view_controller *,
		      const struct morph_tui_widget_action *);
	int (*event)(struct morph_tui_core_view_controller *, const struct morph_tui_core_event *);
	int (*appear)(struct morph_tui_core_view_controller *);
	void (*disappear)(struct morph_tui_core_view_controller *);
	void (*destroy)(struct morph_tui_core_view_controller *, int result);
};

/* Embed first in custom controllers. Instances and their view scopes are
 * framework-owned. data is borrowed through destroy, including build failure.
 * destroy runs after view destruction; it must not access released views. */
struct morph_tui_core_view_controller {
	const struct morph_tui_core_view_controller_ops *ops;
	void *data;
	struct morph_tui_core_view_controller_state *state;
};

struct morph_tui_core_view_controller_spec {
	const struct morph_tui_core_view_controller_ops *ops;
	void *data;
	size_t size;
	/* Optional printable page shortcut, after the focused view declines it. */
	int quit_key;
	/* Optional business refresh timer. Zero disables periodic callbacks. */
	int tick_ms;
};

int morph_tui_core_view_controller_present(struct morph_tui_core_app *app,
					   const struct morph_tui_core_view_controller_spec *spec);
int morph_tui_core_view_controller_dismiss(struct morph_tui_core_view_controller *controller,
					   int result);
struct morph_tui_core_app *
morph_tui_core_view_controller_app(const struct morph_tui_core_view_controller *controller);

#ifdef __cplusplus
}
#endif
#endif
