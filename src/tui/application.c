#include <morph/tui/application.h>
#include "util/array.h"
#include "util/error.h"
#include "util/queue.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <uv.h>

#define TUI_APP_EVENT_BUDGET 64
#define TUI_APP_RESIZE_MS 100

struct app_controller {
	struct morph_tui_controller descriptor;
	uint64_t id;
	int64_t tick_at;
	int visible;
};

struct app_transition {
	struct app_controller controller;
	uint64_t dismiss_id;
	int result;
};

struct app_message {
	morph_queue_t link;
	int code;
	void *payload;
	void (*release)(void *payload);
};

struct morph_tui_app {
	struct morph_tui_app_options options;
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
	int input_enabled;
	int result;
	unsigned invalid;
	uint64_t sequence;
	int64_t scheduled_at;
	int64_t resize_at;
};

static int64_t app_now(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static struct app_controller *app_top(const struct morph_tui_app *app)
{
	return app->controllers.nelts ?
		morph_array_get(&app->controllers, app->controllers.nelts - 1) : NULL;
}

int morph_tui_app_is_owner(const struct morph_tui_app *app)
{
	return app && pthread_equal(app->owner, pthread_self());
}

static int app_system_event(enum morph_tui_event_type type)
{
	return type != MORPH_TUI_EVENT_KEY && type != MORPH_TUI_EVENT_INPUT_READY;
}

void morph_tui_app_stop(struct morph_tui_app *app, int result)
{
	if (!morph_tui_app_is_owner(app))
		return;
	if (!app->stopping)
		app->result = result;
	app->stopping = 1;
	pthread_mutex_lock(&app->mutex);
	app->accepting = 0;
	pthread_mutex_unlock(&app->mutex);
	uv_stop(&app->loop);
}

static void app_dispatch(struct morph_tui_app *app, const struct morph_tui_event *event)
{
	if (app->stopping)
		return;
	int rc = 0;

	if (app_system_event(event->type) && app->options.event)
		rc = app->options.event(app->options.data, app, event);
	struct app_controller *top = app_top(app);

	if (!rc && !app->stopping && top && top->descriptor.ops->event &&
	    !app->transitions.nelts)
		rc = top->descriptor.ops->event(top->descriptor.data, app, event);
	if (rc < 0)
		morph_tui_app_stop(app, rc);
}

static void app_probe_size(struct morph_tui_app *app)
{
	int columns, rows;

	morph_tui_dimensions(app->screen.output_fd, &columns, &rows);
	app->resize_at = app_now() + TUI_APP_RESIZE_MS;
	if (columns == app->screen.area.width && rows == app->screen.area.height)
		return;
	app->screen.area.width = columns;
	app->screen.area.height = rows;
	app->invalid |= MORPH_TUI_INVALIDATE_LAYOUT | MORPH_TUI_INVALIDATE_DISPLAY;
	struct morph_tui_event event = {.type = MORPH_TUI_EVENT_RESIZE};

	app_dispatch(app, &event);
}

static void app_read_key(struct morph_tui_app *app)
{
	int key = morph_tui_input_read(&app->screen.input,
		fileno(app->options.input), 0);
	struct morph_tui_event event = {.type = MORPH_TUI_EVENT_KEY};

	if (key == -ECANCELED) {
		event.type = MORPH_TUI_EVENT_EOF;
		app_dispatch(app, &event);
		if (!app->transitions.nelts)
			(void)morph_tui_app_dismiss(app, 0);
		return;
	}
	if (key == -EINTR || key == -EAGAIN)
		return;
	if (key < 0) {
		morph_tui_app_stop(app, key);
		return;
	}
	if (!key)
		return;
	if (key == MORPH_TUI_KEY_PASTE_BEGIN)
		app->screen.pasting = 1;
	event.key = key;
	event.pasting = app->screen.pasting;
	app_dispatch(app, &event);
	if (key == MORPH_TUI_KEY_PASTE_END)
		app->screen.pasting = 0;
}

static void app_input_ready(uv_poll_t *handle, int status, int events)
{
	struct morph_tui_app *app = handle->data;
	struct app_controller *top = app_top(app);

	if (status < 0) {
		morph_tui_app_stop(app, status);
		return;
	}
	if (!top || app->transitions.nelts || app->stopping ||
	    !(events & (UV_READABLE | UV_DISCONNECT)))
		return;
	if (top->descriptor.input_mode == MORPH_TUI_INPUT_KEYS)
		app_read_key(app);
	else {
		struct morph_tui_event event = {.type = MORPH_TUI_EVENT_INPUT_READY};

		app_dispatch(app, &event);
	}
}

static void app_woken(uv_async_t *handle)
{
	struct morph_tui_app *app = handle->data;
	struct morph_tui_event wake = {.type = MORPH_TUI_EVENT_WAKE};

	app_dispatch(app, &wake);
	for (int i = 0; i < TUI_APP_EVENT_BUDGET && !app->stopping; i++) {
		pthread_mutex_lock(&app->mutex);
		morph_queue_t *link = morph_queue_empty(&app->messages) ? NULL :
			app->messages.next;

		if (link)
			morph_queue_remove(link);
		pthread_mutex_unlock(&app->mutex);
		if (!link)
			break;
		struct app_message *message = morph_queue_data(link, struct app_message, link);
		struct morph_tui_event event = {
			.type = MORPH_TUI_EVENT_MESSAGE, .code = message->code,
			.data = message->payload,
		};

		app_dispatch(app, &event);
		if (message->release)
			message->release(message->payload);
		free(message);
	}
	pthread_mutex_lock(&app->mutex);
	if (!morph_queue_empty(&app->messages) && app->accepting)
		(void)uv_async_send(&app->wake);
	pthread_mutex_unlock(&app->mutex);
}

static void app_timer_fired(uv_timer_t *handle)
{
	struct morph_tui_app *app = handle->data;
	struct app_controller *top = app_top(app);
	int64_t now = app_now();
	int tick = app->scheduled_at >= 0 && now >= app->scheduled_at;

	if (tick)
		app->scheduled_at = -1;
	if (top && top->descriptor.tick_ms > 0 && now >= top->tick_at) {
		top->tick_at = now + top->descriptor.tick_ms;
		tick = 1;
	}
	if (top && top->descriptor.input_mode == MORPH_TUI_INPUT_KEYS) {
		if (now >= app->resize_at)
			app_probe_size(app);
		if (app->screen.input.deadline_ms > 0 &&
		    now >= app->screen.input.deadline_ms)
			app_read_key(app);
	}
	if (tick) {
		struct morph_tui_event event = {.type = MORPH_TUI_EVENT_TICK};

		app_dispatch(app, &event);
	}
}

static void app_arm_timer(struct morph_tui_app *app)
{
	struct app_controller *top = app_top(app);
	int64_t deadline = app->scheduled_at;

	if (top && top->descriptor.tick_ms > 0 &&
	    (deadline < 0 || top->tick_at < deadline))
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
	int64_t remaining = deadline - app_now();

	(void)uv_timer_start(&app->timer, app_timer_fired,
		remaining > 0 ? (uint64_t)remaining : 0, 0);
}

void morph_tui_app_schedule(struct morph_tui_app *app, int delay_ms)
{
	if (morph_tui_app_is_owner(app))
		app->scheduled_at = delay_ms < 0 ? -1 : app_now() + delay_ms;
}

void morph_tui_app_invalidate(struct morph_tui_app *app, unsigned flags)
{
	if (morph_tui_app_is_owner(app))
		app->invalid |= flags;
}

void morph_tui_app_set_input_enabled(struct morph_tui_app *app, int enabled)
{
	if (!morph_tui_app_is_owner(app))
		return;
	app->input_enabled = enabled != 0;
	if (enabled) {
		int rc = uv_poll_start(&app->input, UV_READABLE, app_input_ready);

		if (rc < 0)
			morph_tui_app_stop(app, rc);
	} else {
		(void)uv_poll_stop(&app->input);
	}
}

static int app_terminal_mode(struct morph_tui_app *app)
{
	struct app_controller *top = app_top(app);
	int raw = top && top->descriptor.input_mode == MORPH_TUI_INPUT_KEYS;
	int flags = raw ? top->descriptor.terminal_flags : 0;

	if (app->screen.session.active && raw &&
	    app->screen.session.flags == flags)
		return 0;
	int rc = morph_tui_session_leave(&app->screen.session);

	app->screen.input = (struct morph_tui_input){0};
	app->screen.pasting = 0;
	if (!rc && raw)
		rc = morph_tui_session_enter(&app->screen.session,
			app->options.input, app->options.output, flags);
	if (!rc && fcntl(fileno(app->options.input), F_SETFL,
		app->saved_flags) < 0)
		rc = -errno;
	return rc;
}

int morph_tui_app_present(struct morph_tui_app *app,
			 const struct morph_tui_controller *controller)
{
	if (!morph_tui_app_is_owner(app))
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
	struct app_transition *transition = morph_array_push(&app->transitions);

	if (!transition)
		return -ENOMEM;
	*transition = (struct app_transition){.controller = {
		.descriptor = *controller, .id = ++app->sequence,
	}};
	return 0;
}

int morph_tui_app_dismiss(struct morph_tui_app *app, int result)
{
	if (!morph_tui_app_is_owner(app))
		return -EPERM;
	struct app_controller *top = app_top(app);

	if (!top || app->stopping)
		return -ECANCELED;
	struct app_transition *transition = morph_array_push(&app->transitions);

	if (!transition)
		return -ENOMEM;
	*transition = (struct app_transition){.dismiss_id = top->id, .result = result};
	return 0;
}

size_t morph_tui_app_depth(const struct morph_tui_app *app)
{
	return app ? app->controllers.nelts : 0;
}

int morph_tui_app_transition_pending(const struct morph_tui_app *app)
{
	return app && app->transitions.nelts != 0;
}

static void app_disappear(struct morph_tui_app *app)
{
	struct app_controller *top = app_top(app);

	if (!top || !top->visible)
		return;
	top->visible = 0;
	if (top->descriptor.ops->disappear)
		top->descriptor.ops->disappear(top->descriptor.data, app);
}

static int app_appear(struct morph_tui_app *app)
{
	int rc = app_terminal_mode(app);
	struct app_controller *top = app_top(app);

	morph_tui_dimensions(app->screen.output_fd,
		&app->screen.area.width, &app->screen.area.height);
	app->invalid |= MORPH_TUI_INVALIDATE_LAYOUT | MORPH_TUI_INVALIDATE_DISPLAY;
	app->resize_at = app_now() + TUI_APP_RESIZE_MS;
	if (top) {
		top->tick_at = app_now() + top->descriptor.tick_ms;
		top->visible = rc == 0;
		if (!rc && top->descriptor.ops->appear)
			rc = top->descriptor.ops->appear(top->descriptor.data, app);
	}
	return rc;
}

static int app_transitions_commit(struct morph_tui_app *app)
{
	size_t count = app->transitions.nelts;
	size_t processed = 0;

	/* Callback-generated transitions wait for the next step. Element pointers
	 * cannot survive callbacks, which may grow the transition array. */
	for (size_t i = 0; i < count && !app->stopping; i++) {
		struct app_transition transition =
			*(struct app_transition *)morph_array_get(&app->transitions, i);
		struct app_controller *top = app_top(app);

		if (transition.dismiss_id && (!top || top->id != transition.dismiss_id)) {
			processed = i + 1;
			continue;
		}
		struct morph_tui_controller old = top ? top->descriptor :
			(struct morph_tui_controller){0};

		app_disappear(app);
		if (app->stopping)
			break;
		if (transition.dismiss_id) {
			morph_array_pop(&app->controllers);
			int rc = app_terminal_mode(app);

			if (old.ops->close)
				old.ops->close(old.data, transition.result);
			if (rc)
				return rc;
		} else {
			struct app_controller *entry = morph_array_push(&app->controllers);

			if (!entry)
				return -ENOMEM;
			*entry = transition.controller;
		}
		/* Mark a presented transition consumed before callbacks can fail. */
		struct app_transition *consumed = morph_array_get(&app->transitions, i);

		consumed->controller.id = 0;
		processed = i + 1;
		if (app->stopping)
			break;
		int rc = app_appear(app);

		if (rc)
			return rc;
		if (!app->controllers.nelts)
			morph_tui_app_stop(app, transition.result);
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

static int app_render(struct morph_tui_app *app)
{
	if (!app->stopping)
		app_probe_size(app);
	struct app_controller *top = app_top(app);

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

int morph_tui_app_step(struct morph_tui_app *app, int wait)
{
	if (!morph_tui_app_is_owner(app))
		return -EPERM;
	if (app->stepping)
		return -EBUSY;
	if (app->stopping)
		return app->result;
	app->stepping = 1;
	int rc = app_transitions_commit(app);

	if (!rc)
		rc = app_render(app);
	if (!rc && !app->stopping) {
		app_arm_timer(app);
		(void)uv_run(&app->loop, wait && !app->transitions.nelts ?
			UV_RUN_ONCE : UV_RUN_NOWAIT);
		rc = app_transitions_commit(app);
		if (!rc)
			rc = app_render(app);
	}
	if (rc < 0)
		morph_tui_app_stop(app, rc);
	app->stepping = 0;
	return app->stopping ? app->result : rc;
}

int morph_tui_app_run(struct morph_tui_app *app)
{
	if (!morph_tui_app_is_owner(app))
		return -EPERM;
	if (app->stepping)
		return -EBUSY;
	if (!app->controllers.nelts && !app->transitions.nelts)
		return -EINVAL;
	while (!app->stopping)
		(void)morph_tui_app_step(app, 1);
	return app->result;
}

int morph_tui_app_wake(struct morph_tui_app *app)
{
	if (!app)
		return -EINVAL;
	pthread_mutex_lock(&app->mutex);
	int rc = app->accepting ? uv_async_send(&app->wake) : -ECANCELED;

	pthread_mutex_unlock(&app->mutex);
	return rc;
}

int morph_tui_app_post(struct morph_tui_app *app, int code, void *payload,
		       void (*release)(void *payload))
{
	if (!app)
		return -EINVAL;
	struct app_message *message = calloc(1, sizeof(*message));

	if (!message)
		return -ENOMEM;
	message->code = code;
	message->payload = payload;
	message->release = release;
	pthread_mutex_lock(&app->mutex);
	int rc = app->accepting ? uv_async_send(&app->wake) : -ECANCELED;

	if (!rc)
		morph_queue_insert_tail(&app->messages, &message->link);
	pthread_mutex_unlock(&app->mutex);
	if (rc)
		free(message);
	return rc;
}

int morph_tui_app_create(const struct morph_tui_app_options *options,
			 struct morph_tui_app **out)
{
	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!options || !options->input || !options->output)
		return -EINVAL;
	struct morph_tui_app *app = calloc(1, sizeof(*app));

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
	app->screen.area = (struct morph_tui_rect){.x = 1, .y = 1};
	morph_tui_dimensions(app->screen.output_fd,
		&app->screen.area.width, &app->screen.area.height);
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
	struct app_controller controller;
	struct app_transition transition;

	rc = morph_array_init(&app->controllers, 8, sizeof(controller));
	if (!rc)
		rc = morph_array_init(&app->transitions, 8, sizeof(transition));
	if (!rc) {
		rc = uv_async_init(&app->loop, &app->wake, app_woken);
		if (!rc) app->initialized = 1;
	}
	if (!rc) {
		rc = uv_timer_init(&app->loop, &app->timer);
		if (!rc) app->initialized = 2;
	}
	if (!rc) {
		rc = uv_poll_init(&app->loop, &app->input, fileno(options->input));
		if (!rc) app->initialized = 3;
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
	if (rc) {
		morph_tui_app_destroy(app);
		return rc;
	}
	app->accepting = 1;
	morph_tui_app_set_input_enabled(app, 1);
	if (app->stopping) {
		rc = app->result;
		morph_tui_app_destroy(app);
		return rc;
	}
	*out = app;
	return 0;
}

void morph_tui_app_destroy(struct morph_tui_app *app)
{
	if (!morph_tui_app_is_owner(app) || app->stepping)
		return;
	morph_tui_app_stop(app, -ECANCELED);
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
	app_disappear(app);
	(void)morph_tui_session_leave(&app->screen.session);
	int close_result = app->result < 0 ? app->result : -ECANCELED;

	while (app->controllers.nelts) {
		struct app_controller controller = *app_top(app);

		morph_array_pop(&app->controllers);
		if (controller.descriptor.ops->close)
			controller.descriptor.ops->close(controller.descriptor.data, close_result);
	}
	for (size_t i = 0; i < app->transitions.nelts; i++) {
		struct app_transition *transition = morph_array_get(&app->transitions, i);
		struct morph_tui_controller *controller = &transition->controller.descriptor;

		if (transition->controller.id && controller->ops->close)
			controller->ops->close(controller->data, close_result);
	}
	while (!morph_queue_empty(&app->messages)) {
		struct app_message *message = morph_queue_data(
			app->messages.next, struct app_message, link);

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
	pthread_mutex_destroy(&app->mutex);
	free(app);
}
