#include <morph/tui/core/runner.h>
#include <morph/tui/core/window.h>
#include "util/error.h"
#include "util/signals.h"

#include <stdlib.h>

struct morph_tui_core_application_state {
	struct morph_tui_core_app *runtime;
	volatile sig_atomic_t signal;
};

struct morph_tui_core_app *
morph_tui_core_application_runtime(const struct morph_tui_core_application *application)
{
	return application && application->state ? application->state->runtime : NULL;
}

static void core_runner_signal(int number, void *data)
{
	struct morph_tui_core_application_state *state = data;

	if (number != SIGWINCH)
		morph_signal_flag_set(&state->signal, number);
	(void)morph_tui_core_app_wake(state->runtime);
}

static int core_runner_event(void *data, struct morph_tui_core_app *runtime,
			     const struct morph_tui_core_event *event)
{
	struct morph_tui_core_application *application = data;
	int number = morph_signal_flag_take(&application->state->signal);

	if (number) {
		struct morph_tui_core_event signal = {.type = MORPH_TUI_CORE_EVENT_SIGNAL,
						      .code = number};
		int rc = application->ops && application->ops->event
				 ? application->ops->event(application, &signal)
				 : 0;

		if (!rc)
			morph_tui_core_app_stop(runtime, 0);
		return rc < 0 ? rc : 1;
	}
	return application->ops && application->ops->event
		       ? application->ops->event(application, event)
		       : 0;
}

static int core_runner_input(void *data, const struct morph_tui_core_event *event)
{
	struct morph_tui_core_application *application = data;

	return application->ops && application->ops->event
		       ? application->ops->event(application, event)
		       : 0;
}

int morph_tui_core_application_run(struct morph_tui_core_application *application,
				   const struct morph_tui_core_application_options *options,
				   const struct morph_tui_core_view_controller_spec *root)
{
	if (!application || application->state || !root)
		return -EINVAL;
	struct morph_tui_core_application_state state = {0};
	struct morph_signal_listener listener = {0};
	struct morph_tui_core_app_options config = {
		.input = options && options->input ? options->input : stdin,
		.output = options && options->output ? options->output : stdout,
		.event = core_runner_event,
		.data = application,
	};
	int rc = morph_tui_terminal_available(config.input, config.output) ? 0 : -ENOTTY;

	application->state = &state;
	if (!rc)
		rc = morph_tui_core_app_create(&config, &state.runtime);
	if (!rc)
		rc = morph_tui_core_window_set_event_handler(
			morph_tui_core_app_window(state.runtime), core_runner_input, application);
	if (!rc && (!options || !options->external_signals))
		rc = morph_signal_listener_start(&listener, core_runner_signal, &state);
	if (!rc)
		rc = morph_tui_core_view_controller_present(state.runtime, root);
	if (!rc && application->ops && application->ops->launch)
		rc = application->ops->launch(application);
	if (!rc)
		rc = morph_tui_core_app_run(state.runtime);
	morph_tui_core_app_stop(state.runtime, rc);
	if (application->ops && application->ops->prepare_termination)
		application->ops->prepare_termination(application, rc);
	morph_signal_listener_stop(&listener);
	morph_tui_core_app_destroy(state.runtime);
	state.runtime = NULL;
	if (application->ops && application->ops->terminate)
		application->ops->terminate(application, rc);
	application->state = NULL;
	return rc;
}

int morph_tui_core_application_main(struct morph_tui_core_application *application,
				    const struct morph_tui_core_view_controller_spec *root)
{
	int rc = morph_tui_core_application_run(application, NULL, root);

	if (rc < 0) {
		fprintf(stderr, "TUI application failed: %s\n", morph_strerror(rc));
		return EXIT_FAILURE;
	}
	return rc;
}
