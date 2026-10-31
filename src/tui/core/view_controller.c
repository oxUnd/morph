#include "view_internal.h"
#include "util/error.h"

#include <stdlib.h>

static int core_view_controller_layout(void *data, const struct morph_tui_terminal_rect *area)
{
	struct morph_tui_view_controller *controller = data;
	struct morph_tui_view_size size;
	int rc = morph_tui_view_measure(controller->state->root, area->width, &size);

	if (!rc)
		rc = morph_tui_view_arrange(
			controller->state->root,
			(struct morph_tui_view_rect){0, 0, area->width, area->height});
	return rc;
}

static int core_view_controller_draw(void *data, const struct morph_tui_screen *screen)
{
	struct morph_tui_view_controller *controller = data;

	return core_window_draw(morph_tui_application_window(controller->state->app), screen);
}

static int core_view_controller_appear(void *data, struct morph_tui_application *app)
{
	struct morph_tui_view_controller *controller = data;
	struct morph_tui_window *window = morph_tui_application_window(app);

	controller->state->visible = 1;
	window->controller = controller;
	core_window_invalidate(window);
	if (!core_view_can_focus(controller->state->focus))
		core_view_focus_next(controller, 0);
	return controller->ops->appear ? controller->ops->appear(controller) : 0;
}

static void core_view_controller_disappear(void *data, struct morph_tui_application *app)
{
	struct morph_tui_view_controller *controller = data;
	struct morph_tui_window *window = morph_tui_application_window(app);

	controller->state->visible = 0;
	if (window->controller == controller)
		window->controller = NULL;
	if (controller->ops->disappear)
		controller->ops->disappear(controller);
}

static void core_view_controller_close(void *data, int result)
{
	struct morph_tui_view_controller *controller = data;

	morph_buf_cleanup(&controller->state->text);
	core_view_scope_destroy(controller);
	if (controller->ops->destroy)
		controller->ops->destroy(controller, result);
	free(controller->state);
	free(controller);
}

static int core_view_controller_dispatch(void *data, struct morph_tui_application *app,
					 const struct morph_tui_event *event)
{
	struct morph_tui_view_controller *controller = data;
	struct morph_tui_view_controller_state *state = controller->state;
	int rc = 0;

	if (event->type == MORPH_TUI_EVENT_KEY || event->type == MORPH_TUI_EVENT_TEXT ||
	    event->type == MORPH_TUI_EVENT_PASTE) {
		if (!core_view_can_focus(state->focus))
			core_view_focus_next(controller, 0);
		if (!event->pasting &&
		    (event->key == '\t' || event->key == MORPH_TUI_KEY_BACK_TAB)) {
			rc = core_view_focus_next(controller, event->key == MORPH_TUI_KEY_BACK_TAB);
			return rc ? rc : 1;
		}
		for (struct morph_tui_view *view = event->pasting ? state->paste_target
								  : state->focus;
		     view && !rc; view = view->state->parent) {
			if (view->ops && view->ops->event)
				rc = view->ops->event(view, event);
			if (morph_tui_application_transition_pending(app))
				return rc < 0 ? rc : 1;
		}
		/* Pasted bytes never reach page shortcuts or raw page handlers. */
		if (event->pasting || event->key == MORPH_TUI_KEY_PASTE_END)
			return rc < 0 ? rc : 1;
	}
	if (!rc && controller->ops->event)
		rc = controller->ops->event(controller, event);
	if (rc || morph_tui_application_transition_pending(app))
		return rc;
	struct morph_tui_window *window = morph_tui_application_window(app);

	if ((event->type == MORPH_TUI_EVENT_KEY || event->type == MORPH_TUI_EVENT_TEXT) &&
	    window->event)
		rc = window->event(window->event_data, event);
	if (rc || morph_tui_application_transition_pending(app))
		return rc;
	if ((event->type == MORPH_TUI_EVENT_KEY || event->type == MORPH_TUI_EVENT_TEXT) &&
	    (event->key == 27 || event->key == 3 || event->key == 4 ||
	     (state->quit_key && event->key == state->quit_key))) {
		rc = morph_tui_view_controller_dismiss(controller, 0);
		return rc ? rc : 1;
	}
	return 0;
}

static int core_view_controller_event(void *data, struct morph_tui_application *app,
				      const struct morph_tui_event *event)
{
	struct morph_tui_view_controller *controller = data;
	struct morph_tui_view_controller_state *state = controller->state;

	if (event->type != MORPH_TUI_EVENT_KEY)
		return core_view_controller_dispatch(data, app, event);
	if (event->key == MORPH_TUI_KEY_PASTE_BEGIN)
		state->paste_target = state->focus;
	struct morph_tui_view *target = event->pasting ? state->paste_target : state->focus;

	if (target != state->text_target || event->key == MORPH_TUI_KEY_PASTE_BEGIN) {
		utf8_terminal_sanitizer_reset(&state->sanitizer);
		state->text_target = target;
	}
	if (event->key >= 32 && event->key < 256 && event->key != 127) {
		char byte = (char)event->key;
		struct morph_tui_event text = *event;

		morph_buf_reset(&state->text);
		int rc = utf8_terminal_sanitize_feed(&state->sanitizer, &state->text, &byte, 1, 0);

		if (rc)
			return rc;
		if (!state->text.len)
			return 1;
		text.type = event->pasting ? MORPH_TUI_EVENT_PASTE : MORPH_TUI_EVENT_TEXT;
		text.text = morph_buf_cstr(&state->text);
		text.text_length = state->text.len;
		return core_view_controller_dispatch(data, app, &text);
	}
	int rc = core_view_controller_dispatch(data, app, event);

	if (event->key == MORPH_TUI_KEY_PASTE_END) {
		state->paste_target = NULL;
		utf8_terminal_sanitizer_reset(&state->sanitizer);
	}
	return rc;
}

int morph_tui_view_controller_present(struct morph_tui_application *app,
				      const struct morph_tui_view_controller_spec *spec)
{
	if (!morph_tui_application_is_owner(app))
		return -EPERM;
	struct morph_tui_view_controller *controller = NULL;

	if (!spec || !spec->ops || !spec->ops->build || spec->tick_ms < 0 ||
	    (spec->size && spec->size < sizeof(*controller)))
		return -EINVAL;
	size_t size = spec->size ? spec->size : sizeof(*controller);

	controller = calloc(1, size);
	if (!controller)
		return -ENOMEM;
	controller->ops = spec->ops;
	controller->data = spec->data;
	controller->state = calloc(1, sizeof(*controller->state));
	if (!controller->state) {
		free(controller);
		return -ENOMEM;
	}
	struct morph_tui_view_controller_state *state = controller->state;

	state->app = app;
	state->quit_key = spec->quit_key;
	state->builder.controller = controller;
	state->arena = arena_create(BUFSIZ);
	struct morph_tui_view *view;
	int rc = state->arena ? morph_array_init(&state->views, 16, sizeof(view)) : -ENOMEM;

	if (!rc)
		rc = morph_buf_init(&state->text, 16);
	utf8_terminal_sanitizer_init(&state->sanitizer, UTF8_TERMINAL_TEXT_SINGLE_LINE);

	if (!rc) {
		controller->ops->build(controller, &state->builder);
		rc = state->builder.error;
		if (!rc && !state->root)
			rc = -EINVAL;
	}
	static const struct morph_tui_controller_ops ops = {
		.event = core_view_controller_event,
		.layout = core_view_controller_layout,
		.draw = core_view_controller_draw,
		.appear = core_view_controller_appear,
		.disappear = core_view_controller_disappear,
		.close = core_view_controller_close,
	};
	struct morph_tui_controller descriptor = {
		.ops = &ops,
		.data = controller,
		.input_mode = MORPH_TUI_INPUT_KEYS,
		.terminal_flags =
			MORPH_TUI_TERMINAL_SESSION_ALTERNATE | MORPH_TUI_TERMINAL_SESSION_PASTE,
		.tick_ms = spec->tick_ms,
	};

	if (!rc)
		rc = morph_tui_application_present_controller(app, &descriptor);
	if (rc)
		core_view_controller_close(controller, rc);
	return rc;
}

int morph_tui_view_controller_dismiss(struct morph_tui_view_controller *controller, int result)
{
	if (!controller || !controller->state->visible)
		return -EBUSY;
	return morph_tui_application_dismiss_controller(controller->state->app, result);
}

struct morph_tui_application *
morph_tui_view_controller_application(const struct morph_tui_view_controller *controller)
{
	return controller ? controller->state->app : NULL;
}
