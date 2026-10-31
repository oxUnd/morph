#ifndef MORPH_TUI_CORE_RUNNER_H
#define MORPH_TUI_CORE_RUNNER_H

#include <morph/tui/core/view_controller.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_core_application;
struct morph_tui_core_application_state;

struct morph_tui_core_application_ops {
	int (*launch)(struct morph_tui_core_application *);
	int (*event)(struct morph_tui_core_application *, const struct morph_tui_core_event *);
	void (*terminate)(struct morph_tui_core_application *, int result);
	/* Runtime is stopped but alive. Join producers here before destruction. */
	void (*prepare_termination)(struct morph_tui_core_application *, int result);
};

/* Embed first in a custom application. Zero-init for the default application.
 * The caller owns this instance; state exists only during run/main. */
struct morph_tui_core_application {
	const struct morph_tui_core_application_ops *ops;
	struct morph_tui_core_application_state *state;
};

struct morph_tui_core_application_options {
	FILE *input, *output;
	/* Host owns process signal policy. Its workers post events to runtime. */
	int external_signals;
};

struct morph_tui_core_app *
morph_tui_core_application_runtime(const struct morph_tui_core_application *application);
int morph_tui_core_application_run(struct morph_tui_core_application *application,
				   const struct morph_tui_core_application_options *options,
				   const struct morph_tui_core_view_controller_spec *root);
/* Reports framework errors after terminal cleanup and returns an exit status. */
int morph_tui_core_application_main(struct morph_tui_core_application *application,
				    const struct morph_tui_core_view_controller_spec *root);

#ifdef __cplusplus
}
#endif
#endif
