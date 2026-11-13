#include "application_loop.h"
#include "view_internal.h"
#include "util/array.h"
#include "util/error.h"
#include "util/queue.h"

#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <uv.h>

#define CORE_APP_EVENT_BUDGET 64
#define CORE_APP_RESIZE_MS 100

struct core_app_controller {
	struct morph_tui_controller descriptor;
	uint64_t id;
	int64_t tick_at;
	int visible;
};

struct core_app_transition {
	struct core_app_controller controller;
	uint64_t dismiss_id;
	int result;
};

struct core_app_message {
	morph_queue_t link;
	int code;
	void *payload;
	void (*release)(void *payload);
};

struct core_application_loop {
	struct morph_tui_window *window;
	struct core_application_loop_options options;
	uv_loop_t loop;
	uv_poll_t input;
	uv_async_t wake;
	uv_timer_t timer;
	pthread_t owner;
	pthread_mutex_t mutex;
	morph_queue_t messages;
	morph_array_t controllers;
	morph_array_t transitions;
	struct morph_tui_screen screen;
	struct termios saved_termios;
	int saved_flags;
	int saved_tty;
	int initialized;
	int accepting;
	int stopping;
	int stepping;
	int rendering;
	int input_enabled;
	int result;
	unsigned invalid;
	uint64_t sequence;
	int64_t scheduled_at;
	int64_t resize_at;
};

static int64_t core_app_now(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static struct core_app_controller *core_app_top(const struct core_application_loop *app)
{
	return app->controllers.nelts
		       ? morph_array_borrow(&app->controllers, app->controllers.nelts - 1)
		       : NULL;
}

int core_application_loop_is_owner(const struct core_application_loop *app)
{
	return app && pthread_equal(app->owner, pthread_self());
}

struct morph_tui_window *core_application_loop_window(struct core_application_loop *app)
{
	return app ? app->window : NULL;
}

int core_application_loop_busy(const struct core_application_loop *app)
{
	return app && app->stepping;
}

int core_application_loop_result(const struct core_application_loop *app)
{
	return app ? app->result : -EINVAL;
}

static int core_app_system_event(enum morph_tui_event_type type)
{
	return type != MORPH_TUI_EVENT_KEY && type != MORPH_TUI_EVENT_INPUT_READY;
}

void core_application_loop_stop(struct core_application_loop *app, int result)
{
	if (!core_application_loop_is_owner(app))
		return;
	if (!app->stopping)
		app->result = result;
	app->stopping = 1;
	pthread_mutex_lock(&app->mutex);
	app->accepting = 0;
	pthread_mutex_unlock(&app->mutex);
	uv_stop(&app->loop);
}

static void core_app_dispatch(struct core_application_loop *app,
			      const struct morph_tui_event *event)
{
	if (app->stopping)
		return;
	int rc = 0;

	if (core_app_system_event(event->type) && app->options.event)
		rc = app->options.event(app->options.data, app->options.application, event);
	struct core_app_controller *top = core_app_top(app);

	if (!rc && !app->stopping && top && top->descriptor.ops->event && !app->transitions.nelts)
		rc = top->descriptor.ops->event(top->descriptor.data, app->options.application,
						event);
	if (rc < 0)
		core_application_loop_stop(app, rc);
}

static void core_app_probe_size(struct core_application_loop *app)
{
	int columns, rows;

	morph_tui_terminal_dimensions(app->screen.output_fd, &columns, &rows);
	app->resize_at = core_app_now() + CORE_APP_RESIZE_MS;
	if (columns == app->screen.area.width && rows == app->screen.area.height)
		return;
	app->screen.area.width = columns;
	app->screen.area.height = rows;
	app->invalid |= MORPH_TUI_INVALIDATE_LAYOUT | MORPH_TUI_INVALIDATE_DISPLAY;
	struct morph_tui_event event = {.type = MORPH_TUI_EVENT_RESIZE};

	core_app_dispatch(app, &event);
}

static void core_app_read_key(struct core_application_loop *app)
{
	int key = morph_tui_input_read(&app->screen.input, fileno(app->options.input), 0);
	struct morph_tui_event event = {.type = MORPH_TUI_EVENT_KEY};

	if (key == -ECANCELED) {
		event.type = MORPH_TUI_EVENT_EOF;
		core_app_dispatch(app, &event);
		if (!app->transitions.nelts)
			(void)core_application_loop_dismiss(app, 0);
		return;
	}
	if (key == -EINTR || key == -EAGAIN)
		return;
	if (key < 0) {
		core_application_loop_stop(app, key);
		return;
	}
	if (!key)
		return;
	if (key == MORPH_TUI_KEY_PASTE_BEGIN)
		app->screen.pasting = 1;
	event.key = key;
	event.pasting = app->screen.pasting;
	core_app_dispatch(app, &event);
	if (key == MORPH_TUI_KEY_PASTE_END)
		app->screen.pasting = 0;
}

static void core_app_input_ready(uv_poll_t *handle, int status, int events)
{
	struct core_application_loop *app = handle->data;
	struct core_app_controller *top = core_app_top(app);

	if (status < 0) {
		core_application_loop_stop(app, status);
		return;
	}
	if (!top || app->transitions.nelts || app->stopping ||
	    !(events & (UV_READABLE | UV_DISCONNECT)))
		return;
	if (top->descriptor.input_mode == MORPH_TUI_INPUT_KEYS) {
		for (int i = 0; i < CORE_APP_EVENT_BUDGET; i++) {
			core_app_read_key(app);
			if (app->transitions.nelts || app->stopping)
				break;
			struct pollfd input = {.fd = fileno(app->options.input), .events = POLLIN};

			if (poll(&input, 1, 0) <= 0 || !(input.revents & POLLIN))
				break;
		}
	} else {
		struct morph_tui_event event = {.type = MORPH_TUI_EVENT_INPUT_READY};

		core_app_dispatch(app, &event);
	}
}

static void core_app_woken(uv_async_t *handle)
{
	struct core_application_loop *app = handle->data;
	struct morph_tui_event wake = {.type = MORPH_TUI_EVENT_WAKE};

	core_app_dispatch(app, &wake);
	for (int i = 0; i < CORE_APP_EVENT_BUDGET && !app->stopping; i++) {
		pthread_mutex_lock(&app->mutex);
		morph_queue_t *link = morph_queue_empty(&app->messages) ? NULL : app->messages.next;

		if (link)
			morph_queue_remove(link);
		pthread_mutex_unlock(&app->mutex);
		if (!link)
			break;
		struct core_app_message *message =
			morph_queue_data(link, struct core_app_message, link);
		struct morph_tui_event event = {
			.type = MORPH_TUI_EVENT_MESSAGE,
			.code = message->code,
			.data = message->payload,
		};

		core_app_dispatch(app, &event);
		if (message->release)
			message->release(message->payload);
		free(message);
	}
	pthread_mutex_lock(&app->mutex);
	if (!morph_queue_empty(&app->messages) && app->accepting)
		(void)uv_async_send(&app->wake);
	pthread_mutex_unlock(&app->mutex);
}

static void core_app_timer_fired(uv_timer_t *handle)
{
	struct core_application_loop *app = handle->data;
	struct core_app_controller *top = core_app_top(app);
	int64_t now = core_app_now();
	int tick = app->scheduled_at >= 0 && now >= app->scheduled_at;

	if (tick)
		app->scheduled_at = -1;
	if (top && top->descriptor.tick_ms > 0 && now >= top->tick_at) {
		top->tick_at = now + top->descriptor.tick_ms;
		tick = 1;
	}
	if (top && top->descriptor.input_mode == MORPH_TUI_INPUT_KEYS) {
		if (now >= app->resize_at)
			core_app_probe_size(app);
		if (app->screen.input.deadline_ms > 0 && now >= app->screen.input.deadline_ms)
			core_app_read_key(app);
	}
	if (tick) {
		struct morph_tui_event event = {.type = MORPH_TUI_EVENT_TICK};

		core_app_dispatch(app, &event);
	}
}

static void core_app_arm_timer(struct core_application_loop *app)
{
	struct core_app_controller *top = core_app_top(app);
	int64_t deadline = app->scheduled_at;

	if (top && top->descriptor.tick_ms > 0 && (deadline < 0 || top->tick_at < deadline))
		deadline = top->tick_at;
	if (top && top->descriptor.input_mode == MORPH_TUI_INPUT_KEYS) {
		if (deadline < 0 || app->resize_at < deadline)
			deadline = app->resize_at;
		if (app->screen.input.deadline_ms > 0 &&
		    (deadline < 0 || app->screen.input.deadline_ms < deadline))
			deadline = app->screen.input.deadline_ms;
	}
	if (deadline < 0) {
		(void)uv_timer_stop(&app->timer);
		return;
	}
	int64_t remaining = deadline - core_app_now();

	(void)uv_timer_start(&app->timer, core_app_timer_fired,
			     remaining > 0 ? (uint64_t)remaining : 0, 0);
}

void core_application_loop_schedule(struct core_application_loop *app, int delay_ms)
{
	if (core_application_loop_is_owner(app))
		app->scheduled_at = delay_ms < 0 ? -1 : core_app_now() + delay_ms;
}

void core_application_loop_invalidate(struct core_application_loop *app, unsigned flags)
{
	if (core_application_loop_is_owner(app))
		app->invalid |= flags;
}

void core_application_loop_set_input_enabled(struct core_application_loop *app, int enabled)
{
	if (!core_application_loop_is_owner(app))
		return;
	app->input_enabled = enabled != 0;
	if (enabled) {
		int rc = uv_poll_start(&app->input, UV_READABLE, core_app_input_ready);

		if (rc < 0)
			core_application_loop_stop(app, rc);
	} else {
		(void)uv_poll_stop(&app->input);
	}
}

static int core_app_terminal_mode(struct core_application_loop *app)
{
	struct core_app_controller *top = core_app_top(app);
	int raw = top && top->descriptor.input_mode == MORPH_TUI_INPUT_KEYS;
	int flags = raw ? top->descriptor.terminal_flags : 0;

	if (app->screen.session.active && raw && app->screen.session.flags == flags)
		return 0;
	int rc = morph_tui_terminal_session_leave(&app->screen.session);

	app->screen.input = (struct morph_tui_input){0};
	app->screen.pasting = 0;
	if (!rc && raw)
		rc = morph_tui_terminal_session_enter(&app->screen.session, app->options.input,
						      app->options.output, flags);
	if (!rc && fcntl(fileno(app->options.input), F_SETFL, app->saved_flags) < 0)
		rc = -errno;
	return rc;
}

int core_application_loop_present_take(struct core_application_loop *app,
				       struct morph_tui_controller *controller)
{
	if (!core_application_loop_is_owner(app))
		return -EPERM;
	if (!controller || !controller->ops || controller->tick_ms < 0 ||
	    (controller->input_mode != MORPH_TUI_INPUT_EXTERNAL &&
	     controller->input_mode != MORPH_TUI_INPUT_KEYS))
		return -EINVAL;
	if (app->stopping)
		return -ECANCELED;
	int rc = morph_array_reserve(&app->controllers,
				     app->controllers.nelts + app->transitions.nelts + 1);

	if (rc)
		return rc;
	struct core_app_transition *transition = morph_array_push_borrow(&app->transitions);

	if (!transition)
		return -ENOMEM;
	*transition = (struct core_app_transition){.controller = {
							   .descriptor = *controller,
							   .id = ++app->sequence,
						   }};
	memset(controller, 0, sizeof(*controller));
	return 0;
}

int core_application_loop_dismiss(struct core_application_loop *app, int result)
{
	if (!core_application_loop_is_owner(app))
		return -EPERM;
	struct core_app_controller *top = core_app_top(app);

	if (!top || app->stopping)
		return -ECANCELED;
	struct core_app_transition *transition = morph_array_push_borrow(&app->transitions);

	if (!transition)
		return -ENOMEM;
	*transition = (struct core_app_transition){.dismiss_id = top->id, .result = result};
	return 0;
}

size_t core_application_loop_depth(const struct core_application_loop *app)
{
	return app ? app->controllers.nelts : 0;
}

int core_application_loop_transition_pending(const struct core_application_loop *app)
{
	return app && app->transitions.nelts != 0;
}

static void core_app_disappear(struct core_application_loop *app)
{
	struct core_app_controller *top = core_app_top(app);

	if (!top || !top->visible)
		return;
	top->visible = 0;
	if (top->descriptor.ops->disappear)
		top->descriptor.ops->disappear(top->descriptor.data, app->options.application);
}

static int core_app_appear(struct core_application_loop *app)
{
	int rc = core_app_terminal_mode(app);
	struct core_app_controller *top = core_app_top(app);

	morph_tui_terminal_dimensions(app->screen.output_fd, &app->screen.area.width,
				      &app->screen.area.height);
	app->invalid |= MORPH_TUI_INVALIDATE_LAYOUT | MORPH_TUI_INVALIDATE_DISPLAY;
	app->resize_at = core_app_now() + CORE_APP_RESIZE_MS;
	if (top) {
		top->tick_at = core_app_now() + top->descriptor.tick_ms;
		top->visible = rc == 0;
		if (!rc && top->descriptor.ops->appear)
			rc = top->descriptor.ops->appear(top->descriptor.data,
							 app->options.application);
	}
	return rc;
}

static int core_app_transitions_commit(struct core_application_loop *app)
{
	size_t count = app->transitions.nelts;
	size_t processed = 0;

	/* Callback-generated transitions wait for the next step. Element pointers
	 * cannot survive callbacks, which may grow the transition array. */
	for (size_t i = 0; i < count && !app->stopping; i++) {
		struct core_app_transition transition =
			*(struct core_app_transition *)morph_array_borrow(&app->transitions, i);
		struct core_app_controller *top = core_app_top(app);

		if (transition.dismiss_id && (!top || top->id != transition.dismiss_id)) {
			processed = i + 1;
			continue;
		}
		struct morph_tui_controller old =
			top ? top->descriptor : (struct morph_tui_controller){0};

		core_app_disappear(app);
		if (app->stopping)
			break;
		if (transition.dismiss_id) {
			morph_array_pop(&app->controllers);
			int rc = core_app_terminal_mode(app);

			if (old.ops->close)
				old.ops->close(old.data, transition.result);
			if (rc)
				return rc;
		} else {
			struct core_app_controller *entry =
				morph_array_push_borrow(&app->controllers);

			if (!entry)
				return -ENOMEM;
			*entry = transition.controller;
		}
		/* Mark a presented transition consumed before callbacks can fail. */
		struct core_app_transition *consumed = morph_array_borrow(&app->transitions, i);

		consumed->controller.id = 0;
		processed = i + 1;
		if (app->stopping)
			break;
		int rc = core_app_appear(app);

		if (rc)
			return rc;
		if (!app->controllers.nelts)
			core_application_loop_stop(app, transition.result);
	}
	if (processed) {
		size_t remaining = app->transitions.nelts - processed;

		if (remaining)
			memmove(app->transitions.elts,
				(char *)app->transitions.elts + processed * app->transitions.size,
				remaining * app->transitions.size);
		app->transitions.nelts = remaining;
	}
	return 0;
}

static int core_app_render(struct core_application_loop *app, int probe)
{
	if (probe && !app->stopping)
		core_app_probe_size(app);
	struct core_app_controller *top = core_app_top(app);

	if (!top || app->stopping || app->transitions.nelts)
		return 0;
	unsigned invalid = app->invalid;
	struct morph_tui_controller controller = top->descriptor;

	app->invalid = 0;
	int rc = 0;

	if ((invalid & MORPH_TUI_INVALIDATE_LAYOUT) && controller.ops->layout)
		rc = controller.ops->layout(controller.data, &app->screen.area);
	if (!rc && invalid && !app->transitions.nelts && controller.ops->draw)
		rc = controller.ops->draw(controller.data, &app->screen);
	if (!rc && ferror(app->options.output))
		rc = -EIO;
	return rc;
}

static int core_app_render_guarded(struct core_application_loop *app, int probe)
{
	if (!core_application_loop_is_owner(app))
		return -EPERM;
	if (app->rendering)
		return -EBUSY;
	app->rendering = 1;
	int rc = core_app_render(app, probe);
	app->rendering = 0;
	return rc;
}

int core_application_loop_render(struct core_application_loop *app)
{
	return core_app_render_guarded(app, 0);
}

int core_application_loop_restore_terminal(struct core_application_loop *app)
{
	if (!core_application_loop_is_owner(app))
		return -EPERM;
	if (app->rendering)
		return -EBUSY;
	int rc = morph_tui_terminal_session_reassert(&app->screen.session);
	app->screen.input = (struct morph_tui_input){0};
	app->screen.pasting = 0;
	core_window_invalidate(app->window);
	core_application_loop_invalidate(app, MORPH_TUI_INVALIDATE_LAYOUT);
	return rc;
}

int core_application_loop_step(struct core_application_loop *app, int wait)
{
	if (!core_application_loop_is_owner(app))
		return -EPERM;
	if (app->stepping)
		return -EBUSY;
	if (app->stopping)
		return app->result;
	app->stepping = 1;
	int rc = core_app_transitions_commit(app);

	if (!rc)
		rc = core_app_render_guarded(app, 1);
	if (!rc && !app->stopping) {
		core_app_arm_timer(app);
		(void)uv_run(&app->loop,
			     wait && !app->transitions.nelts ? UV_RUN_ONCE : UV_RUN_NOWAIT);
		rc = core_app_transitions_commit(app);
		if (!rc)
			rc = core_app_render_guarded(app, 1);
	}
	if (rc < 0)
		core_application_loop_stop(app, rc);
	app->stepping = 0;
	return app->stopping ? app->result : rc;
}

int core_application_loop_run(struct core_application_loop *app)
{
	if (!core_application_loop_is_owner(app))
		return -EPERM;
	if (app->stepping)
		return -EBUSY;
	if (!app->controllers.nelts && !app->transitions.nelts)
		return -EINVAL;
	while (!app->stopping)
		(void)core_application_loop_step(app, 1);
	return app->result;
}

int core_application_loop_wake(struct core_application_loop *app)
{
	if (!app)
		return -EINVAL;
	pthread_mutex_lock(&app->mutex);
	int rc = app->accepting ? uv_async_send(&app->wake) : -ECANCELED;

	pthread_mutex_unlock(&app->mutex);
	return rc;
}

int core_application_loop_post_take(struct core_application_loop *app, int code,
				    struct morph_tui_message *source)
{
	if (!app || !source)
		return -EINVAL;
	struct core_app_message *message = calloc(1, sizeof(*message));

	if (!message)
		return -ENOMEM;
	message->code = code;
	message->payload = source->data;
	message->release = source->release;
	pthread_mutex_lock(&app->mutex);
	int rc = app->accepting ? uv_async_send(&app->wake) : -ECANCELED;

	if (!rc) {
		morph_queue_insert_tail(&app->messages, &message->link);
		memset(source, 0, sizeof(*source));
	}
	pthread_mutex_unlock(&app->mutex);
	if (rc)
		free(message);
	return rc;
}

int core_application_loop_create(const struct core_application_loop_options *options,
				 struct core_application_loop **out)
{
	if (!out)
		return -EINVAL;
	if (*out)
		return -EEXIST;
	if (!options || !options->input || !options->output)
		return -EINVAL;
	struct core_application_loop *app = calloc(1, sizeof(*app));

	if (!app)
		return -ENOMEM;
	app->options = *options;
	app->owner = pthread_self();
	app->scheduled_at = -1;
	app->saved_flags = fcntl(fileno(options->input), F_GETFL);
	if (app->saved_flags < 0) {
		int rc = -errno;

		free(app);
		return rc;
	}
	app->saved_tty = tcgetattr(fileno(options->input), &app->saved_termios) == 0;
	app->screen.output_fd = fileno(options->output);
	app->screen.session.output = options->output;
	app->screen.area = (struct morph_tui_terminal_rect){.x = 1, .y = 1};
	morph_tui_terminal_dimensions(app->screen.output_fd, &app->screen.area.width,
				      &app->screen.area.height);
	morph_queue_init(&app->messages);
	int rc = pthread_mutex_init(&app->mutex, NULL);

	if (rc) {
		free(app);
		return -rc;
	}
	rc = uv_loop_init(&app->loop);
	if (rc) {
		pthread_mutex_destroy(&app->mutex);
		free(app);
		return rc;
	}
	struct core_app_controller controller;
	struct core_app_transition transition;

	rc = morph_array_init(&app->controllers, 8, sizeof(controller));
	if (!rc)
		rc = morph_array_init(&app->transitions, 8, sizeof(transition));
	if (!rc) {
		rc = uv_async_init(&app->loop, &app->wake, core_app_woken);
		if (!rc)
			app->initialized = 1;
	}
	if (!rc) {
		rc = uv_timer_init(&app->loop, &app->timer);
		if (!rc)
			app->initialized = 2;
	}
	if (!rc) {
		rc = uv_poll_init(&app->loop, &app->input, fileno(options->input));
		if (!rc)
			app->initialized = 3;
	}
	app->wake.data = app;
	app->timer.data = app;
	app->input.data = app;
	/* uv_poll_init sets O_NONBLOCK on the open file description. A terminal
	 * can share it with stdout, where nonblocking stdio loses large frames.
	 * Restore the caller's flags; decoded reads already check readiness and
	 * this application is the terminal's only input consumer. */
	if (!rc && fcntl(fileno(options->input), F_SETFL, app->saved_flags) < 0)
		rc = -errno;
	if (!rc)
		rc = core_window_create(options->application, &app->window);
	if (rc) {
		core_application_loop_destroy(app);
		return rc;
	}
	app->accepting = 1;
	core_application_loop_set_input_enabled(app, 1);
	if (app->stopping) {
		rc = app->result;
		core_application_loop_destroy(app);
		return rc;
	}
	*out = app;
	return 0;
}

void core_application_loop_destroy(struct core_application_loop *app)
{
	if (!core_application_loop_is_owner(app) || app->stepping)
		return;
	core_application_loop_stop(app, -ECANCELED);
	if (app->initialized >= 3) {
		(void)uv_poll_stop(&app->input);
		uv_close((uv_handle_t *)&app->input, NULL);
	}
	if (app->initialized >= 2) {
		(void)uv_timer_stop(&app->timer);
		uv_close((uv_handle_t *)&app->timer, NULL);
	}
	if (app->initialized >= 1)
		uv_close((uv_handle_t *)&app->wake, NULL);
	core_app_disappear(app);
	core_window_clear_media(app->window);
	(void)morph_tui_terminal_session_leave(&app->screen.session);
	int close_result = app->result < 0 ? app->result : -ECANCELED;

	while (app->controllers.nelts) {
		struct core_app_controller controller = *core_app_top(app);

		morph_array_pop(&app->controllers);
		if (controller.descriptor.ops->close)
			controller.descriptor.ops->close(controller.descriptor.data, close_result);
	}
	for (size_t i = 0; i < app->transitions.nelts; i++) {
		struct core_app_transition *transition = morph_array_borrow(&app->transitions, i);
		struct morph_tui_controller *controller = &transition->controller.descriptor;

		if (transition->controller.id && controller->ops->close)
			controller->ops->close(controller->data, close_result);
	}
	while (!morph_queue_empty(&app->messages)) {
		struct core_app_message *message =
			morph_queue_data(app->messages.next, struct core_app_message, link);

		morph_queue_remove(&message->link);
		if (message->release)
			message->release(message->payload);
		free(message);
	}
	/* libuv handle storage must survive all close callbacks. */
	while (uv_loop_alive(&app->loop))
		(void)uv_run(&app->loop, UV_RUN_NOWAIT);
	(void)uv_loop_close(&app->loop);
	if (app->saved_tty)
		(void)tcsetattr(fileno(app->options.input), TCSANOW, &app->saved_termios);
	(void)fcntl(fileno(app->options.input), F_SETFL, app->saved_flags);
	morph_array_cleanup(&app->controllers);
	morph_array_cleanup(&app->transitions);
	core_window_destroy(app->window);
	pthread_mutex_destroy(&app->mutex);
	free(app);
}
