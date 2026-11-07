#include <morph/tui/application.h>
#include <morph/tui/view_controller.h>
#include <morph/tui/window.h>
#include "application_loop.h"
#include "util/error.h"
#include "util/signals.h"

#include <stdlib.h>

struct morph_tui_application_state {
	struct core_application_loop *loop;
	struct morph_tui_application_options options;
	struct morph_signal_listener signals;
	volatile sig_atomic_t signal;
	int launched, launching, closing, result;
};

static int core_runner_launch(struct morph_tui_application *app);

static struct core_application_loop *core_runner_loop(const struct morph_tui_application *app)
{
	return app && app->state ? app->state->loop : NULL;
}

int morph_tui_application_is_owner(const struct morph_tui_application *app)
{
	return core_application_loop_is_owner(core_runner_loop(app));
}

static void core_runner_signal(int number, void *data)
{
	struct morph_tui_application *app = data;

	if (number != SIGWINCH)
		morph_signal_flag_set(&app->state->signal, number);
	(void)morph_tui_application_wake(app);
}

static int core_runner_event(void *data, struct morph_tui_application *app,
			     const struct morph_tui_event *event)
{
	struct morph_tui_application_state *state = app->state;
	int number = morph_signal_flag_take(&state->signal);

	(void)data;
	if (number) {
		struct morph_tui_event signal = {.type = MORPH_TUI_EVENT_SIGNAL, .code = number};
		int rc = app->ops && app->ops->event ? app->ops->event(app, &signal) : 0;

		if (!rc)
			morph_tui_application_stop(app, 0);
		return rc < 0 ? rc : 1;
	}
	int rc = app->ops && app->ops->event ? app->ops->event(app, event) : 0;

	if (!rc && state->options.event)
		rc = state->options.event(state->options.data, app, event);
	return rc;
}

static int core_runner_input(void *data, const struct morph_tui_event *event)
{
	struct morph_tui_application *app = data;

	return app->ops && app->ops->event ? app->ops->event(app, event) : 0;
}

int morph_tui_application_init(struct morph_tui_application *app,
			       const struct morph_tui_application_options *options)
{
	if (!app)
		return -EINVAL;
	if (app->state)
		return -EBUSY;
	struct morph_tui_application_state *state = calloc(1, sizeof(*state));

	if (!state)
		return -ENOMEM;
	if (options)
		state->options = *options;
	app->state = state;
	struct core_application_loop_options config = {
		.application = app,
		.input = options && options->input ? options->input : stdin,
		.output = options && options->output ? options->output : stdout,
		.event = core_runner_event,
		.data = app,
	};
	int rc = core_application_loop_create(&config, &state->loop);

	if (!rc)
		rc = morph_tui_window_set_event_handler_borrow(
			morph_tui_application_window_borrow(app), core_runner_input, app);
	if (!rc && !state->options.external_signals)
		rc = morph_signal_listener_start(&state->signals, core_runner_signal, app);
	if (rc) {
		morph_signal_listener_stop(&state->signals);
		core_application_loop_destroy(state->loop);
		free(state);
		app->state = NULL;
	}
	if (!rc) {
		rc = core_runner_launch(app);
		if (rc)
			morph_tui_application_deinit(app);
	}
	return rc;
}

static int core_runner_launch(struct morph_tui_application *app)
{
	if (!morph_tui_application_is_owner(app))
		return -EPERM;
	struct morph_tui_application_state *state = app->state;

	if (state->closing)
		return -ECANCELED;
	if (state->launching)
		return -EBUSY;
	if (state->launched)
		return 0;
	state->launching = 1;
	state->launched = 1;
	int rc = app->ops && app->ops->launch ? app->ops->launch(app) : 0;

	state->launching = 0;
	if (rc)
		morph_tui_application_stop(app, rc);
	return rc;
}

int morph_tui_application_run(struct morph_tui_application *app)
{
	int rc = core_runner_launch(app);

	if (rc)
		return rc;
	rc = core_application_loop_run(app->state->loop);
	if (rc != -EBUSY)
		app->state->result = rc;
	return rc;
}

int morph_tui_application_step(struct morph_tui_application *app, int wait)
{
	int rc = core_runner_launch(app);

	if (rc)
		return rc;
	rc = core_application_loop_step(app->state->loop, wait);
	if (rc != -EBUSY)
		app->state->result = rc;
	return rc;
}

void morph_tui_application_stop(struct morph_tui_application *app, int result)
{
	if (!morph_tui_application_is_owner(app))
		return;
	core_application_loop_stop(app->state->loop, result);
	app->state->result = core_application_loop_result(app->state->loop);
}

void morph_tui_application_deinit(struct morph_tui_application *app)
{
	if (!morph_tui_application_is_owner(app) || app->state->closing || app->state->launching ||
	    core_application_loop_busy(app->state->loop))
		return;
	struct morph_tui_application_state *state = app->state;

	state->closing = 1;
	core_application_loop_stop(state->loop, state->result);
	if (app->ops && app->ops->prepare_termination)
		app->ops->prepare_termination(app, state->result);
	morph_signal_listener_stop(&state->signals);
	core_application_loop_destroy(state->loop);
	state->loop = NULL;
	if (app->ops && app->ops->terminate)
		app->ops->terminate(app, state->result);
	app->state = NULL;
	free(state);
}

int morph_tui_application_create(const struct morph_tui_application_options *options,
				 struct morph_tui_application **out)
{
	if (!out)
		return -EINVAL;
	if (*out)
		return -EEXIST;
	struct morph_tui_application *app = calloc(1, sizeof(*app));

	if (!app)
		return -ENOMEM;
	int rc = morph_tui_application_init(app, options);

	if (rc)
		free(app);
	else
		*out = app;
	return rc;
}

void morph_tui_application_destroy(struct morph_tui_application *app)
{
	if (!app)
		return;
	morph_tui_application_deinit(app);
	if (!app->state)
		free(app);
}

int morph_tui_application_wake(struct morph_tui_application *app)
{
	return core_application_loop_wake(core_runner_loop(app));
}

void morph_tui_message_cleanup(struct morph_tui_message *message)
{
	if (!message)
		return;
	if (message->release)
		message->release(message->data);
	memset(message, 0, sizeof(*message));
}

int morph_tui_application_post(struct morph_tui_application *app, int code)
{
	struct morph_tui_message message = {0};

	return morph_tui_application_post_take(app, code, &message);
}

int morph_tui_application_post_take(struct morph_tui_application *app, int code,
				    struct morph_tui_message *message)
{
	return core_application_loop_post_take(core_runner_loop(app), code, message);
}

struct morph_tui_window *morph_tui_application_window_borrow(struct morph_tui_application *app)
{
	return core_application_loop_window(core_runner_loop(app));
}

size_t morph_tui_application_depth(const struct morph_tui_application *app)
{
	return core_application_loop_depth(core_runner_loop(app));
}

int morph_tui_application_transition_pending(const struct morph_tui_application *app)
{
	return core_application_loop_transition_pending(core_runner_loop(app));
}

void morph_tui_application_invalidate(struct morph_tui_application *app, unsigned flags)
{
	core_application_loop_invalidate(core_runner_loop(app), flags);
}

void morph_tui_application_schedule(struct morph_tui_application *app, int delay_ms)
{
	core_application_loop_schedule(core_runner_loop(app), delay_ms);
}

void morph_tui_application_set_input_enabled(struct morph_tui_application *app, int enabled)
{
	core_application_loop_set_input_enabled(core_runner_loop(app), enabled);
}

int morph_tui_application_present_controller_take(struct morph_tui_application *app,
						  struct morph_tui_controller *controller)
{
	return core_application_loop_present_take(core_runner_loop(app), controller);
}

int morph_tui_application_dismiss_controller(struct morph_tui_application *app, int result)
{
	return core_application_loop_dismiss(core_runner_loop(app), result);
}

int morph_tui_application_main(struct morph_tui_application *app,
			       const struct morph_tui_view_controller_spec *root)
{
	int rc = !app || !root ? -EINVAL : app->state ? -EBUSY : 0;
	int initialized = 0;

	if (!rc) {
		rc = morph_tui_terminal_available(stdin, stdout)
			     ? morph_tui_application_init(app, NULL)
			     : -ENOTTY;
		initialized = rc == 0;
	}

	if (!rc)
		rc = morph_tui_view_controller_present_borrow(app, root);
	if (!rc)
		rc = morph_tui_application_run(app);
	if (initialized) {
		if (rc)
			morph_tui_application_stop(app, rc);
		morph_tui_application_deinit(app);
	}
	if (rc < 0) {
		fprintf(stderr, "TUI application failed: %s\n", morph_strerror(rc));
		return EXIT_FAILURE;
	}
	return rc;
}
