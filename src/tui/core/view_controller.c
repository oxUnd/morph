#include "view_internal.h"
#include "errors.h"

#include <stdlib.h>

static void core_view_controller_mouse_cancel(struct morph_tui_view_controller *controller)
{
	struct morph_tui_view *target = controller->state->mouse_target;

	controller->state->mouse_target = NULL;
	if (!target || !target->ops || !target->ops->event)
		return;
	struct morph_tui_event cancel = {
		.type = MORPH_TUI_EVENT_MOUSE,
		.mouse = {.button = MORPH_TUI_MOUSE_LEFT, .action = MORPH_TUI_MOUSE_CANCEL,
			  .x = -1, .y = -1},
	};

	target->ops->event(target, &cancel);
}

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

	return core_window_draw(morph_tui_application_window_borrow(controller->state->app),
				screen);
}

static int core_view_controller_appear(void *data, struct morph_tui_application *app)
{
	struct morph_tui_view_controller *controller = data;
	struct morph_tui_window *window = morph_tui_application_window_borrow(app);

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
	struct morph_tui_window *window = morph_tui_application_window_borrow(app);

	controller->state->visible = 0;
	core_view_controller_mouse_cancel(controller);
	core_window_clear_selection(window);
	if (window->controller == controller)
		window->controller = NULL;
	if (controller->ops->disappear)
		controller->ops->disappear(controller);
}

static void core_view_controller_close(void *data, int result)
{
	struct morph_tui_view_controller *controller = data;

	nb_buf_cleanup(&controller->state->text);
	core_view_scope_destroy(controller);
	if (controller->ops->destroy)
		controller->ops->destroy(controller, result);
	free(controller->state);
	free(controller);
}

static int core_view_controller_mouse(struct morph_tui_view *view,
				      const struct morph_tui_canvas *parent,
				      const struct morph_tui_event *event)
{
	if (view->state->hidden)
		return 0;
	struct morph_tui_canvas canvas, content;
	struct morph_tui_view_rect frame = view->state->frame;
	struct morph_tui_view_rect rect = morph_tui_view_content_rect(view);

	morph_tui_canvas_child_borrow(parent, frame.x, frame.y, frame.width, frame.height,
				      &canvas);
	morph_tui_canvas_child_borrow(&canvas, rect.x, rect.y, rect.width, rect.height, &content);
	if (event->mouse.x < content.left || event->mouse.x >= content.right ||
	    event->mouse.y < content.top || event->mouse.y >= content.bottom)
		return 0;
	struct morph_tui_canvas children = canvas;

	children.left = content.left;
	children.right = content.right;
	children.top = content.top;
	children.bottom = content.bottom;
	children.y -= view->state->scroll_y;
	for (size_t i = view->state->children.len; i > 0; i--) {
		int rc = core_view_controller_mouse(morph_tui_view_child_borrow(view, i - 1),
						    &children, event);

		if (rc)
			return rc;
	}
	if (!view->ops || !view->ops->event)
		return 0;
	struct morph_tui_view_controller *controller = view->state->controller;
	size_t revision = controller->state->structure_revision;
	struct morph_tui_event local = *event;

	local.mouse.x -= content.x;
	local.mouse.y -= content.y;
	if (event->mouse.action == MORPH_TUI_MOUSE_PRESS &&
	    event->mouse.button == MORPH_TUI_MOUSE_LEFT && core_view_can_focus(view)) {
		int rc = morph_tui_view_focus(view);

		if (rc)
			return rc;
	}
	int rc = view->ops->event(view, &local);

	if (revision != controller->state->structure_revision ||
	    morph_tui_application_transition_pending(controller->state->app))
		return rc < 0 ? rc : 1;
	if (rc > 0 && event->mouse.action == MORPH_TUI_MOUSE_PRESS &&
	    event->mouse.button == MORPH_TUI_MOUSE_LEFT)
		controller->state->mouse_target = view;
	return rc;
}

static int core_view_controller_mouse_content(struct morph_tui_view *view,
					      const struct morph_tui_canvas *parent,
					      struct morph_tui_view *target,
					      struct morph_tui_canvas *result)
{
	if (view->state->hidden)
		return 0;
	struct morph_tui_canvas canvas, content;
	struct morph_tui_view_rect frame = view->state->frame;
	struct morph_tui_view_rect rect = morph_tui_view_content_rect(view);

	morph_tui_canvas_child_borrow(parent, frame.x, frame.y, frame.width, frame.height,
				      &canvas);
	morph_tui_canvas_child_borrow(&canvas, rect.x, rect.y, rect.width, rect.height, &content);
	if (view == target) {
		*result = content;
		return 1;
	}
	canvas.left = content.left;
	canvas.right = content.right;
	canvas.top = content.top;
	canvas.bottom = content.bottom;
	canvas.y -= view->state->scroll_y;
	for (size_t i = 0; i < view->state->children.len; i++) {
		if (core_view_controller_mouse_content(morph_tui_view_child_borrow(view, i),
							&canvas, target, result))
			return 1;
	}
	return 0;
}

static int core_view_controller_mouse_captured(struct morph_tui_view_controller *controller,
					       const struct morph_tui_canvas *canvas,
					       const struct morph_tui_event *event)
{
	struct morph_tui_view *target = controller->state->mouse_target;
	struct morph_tui_canvas content;

	if (!core_view_controller_mouse_content(controller->state->root, canvas, target,
						&content)) {
		core_view_controller_mouse_cancel(controller);
		return 1;
	}
	struct morph_tui_event local = *event;

	local.mouse.x -= content.x;
	local.mouse.y -= content.y;
	if (event->mouse.x < content.left || event->mouse.x >= content.right ||
	    event->mouse.y < content.top || event->mouse.y >= content.bottom)
		local.mouse.x = local.mouse.y = -1;
	if (event->mouse.action == MORPH_TUI_MOUSE_RELEASE)
		controller->state->mouse_target = NULL;
	int rc = target->ops->event(target, &local);

	return rc < 0 ? rc : 1;
}

static int core_view_controller_dispatch(void *data, struct morph_tui_application *app,
					 const struct morph_tui_event *event)
{
	struct morph_tui_view_controller *controller = data;
	struct morph_tui_view_controller_state *state = controller->state;
	struct morph_tui_window *window = morph_tui_application_window_borrow(app);
	int rc = 0;

	if (event->type == MORPH_TUI_EVENT_RESIZE || event->type == MORPH_TUI_EVENT_KEY ||
	    event->type == MORPH_TUI_EVENT_TEXT || event->type == MORPH_TUI_EVENT_PASTE) {
		core_window_clear_selection(window);
		core_view_controller_mouse_cancel(controller);
		if (morph_tui_application_transition_pending(app))
			return 1;
	}
	if (event->type == MORPH_TUI_EVENT_MOUSE) {
		if (core_window_selection_dragging(window))
			return core_window_mouse_event(window, event);
		struct morph_tui_canvas canvas;

		morph_tui_canvas_root_borrow(window->surface, &canvas);
		if (state->mouse_target && event->mouse.button == MORPH_TUI_MOUSE_LEFT &&
		    event->mouse.action != MORPH_TUI_MOUSE_PRESS)
			return core_view_controller_mouse_captured(controller, &canvas, event);
		if (event->mouse.action == MORPH_TUI_MOUSE_PRESS) {
			core_view_controller_mouse_cancel(controller);
			if (morph_tui_application_transition_pending(app))
				return 1;
			rc = core_view_controller_mouse(state->root, &canvas, event);
		}
		if (!rc)
			rc = core_window_mouse_event(window, event);
		if (rc || morph_tui_application_transition_pending(app))
			return rc;
	}
	if (event->type == MORPH_TUI_EVENT_KEY || event->type == MORPH_TUI_EVENT_TEXT ||
	    event->type == MORPH_TUI_EVENT_PASTE) {
		if (!core_view_can_focus(state->focus))
			core_view_focus_next(controller, 0);
		if (!event->pasting &&
		    (event->key == '\t' || event->key == MORPH_TUI_KEY_BACK_TAB)) {
			if (state->focus && state->focus->ops && state->focus->ops->event)
				rc = state->focus->ops->event(state->focus, event);
			if (rc)
				return rc;
			rc = core_view_focus_next(controller, event->key == MORPH_TUI_KEY_BACK_TAB);
			return rc ? rc : 1;
		}
		for (struct morph_tui_view *view = event->pasting ? state->paste_target
								  : state->focus;
		     view && !rc;) {
			size_t revision = state->structure_revision;
			if (view->ops && view->ops->event)
				rc = view->ops->event(view, event);
			if (morph_tui_application_transition_pending(app))
				return rc < 0 ? rc : 1;
			if (rc)
				return rc;
			/* An action may replace its own subtree. Do not dereference a
			 * sender or parent released by clear_children during dispatch. */
			if (revision != state->structure_revision) {
				int alive = 0;
				for (size_t i = 0; i < state->views.len; i++) {
					struct morph_tui_view **slot = nb_vec_at(&state->views, i);
					alive |= *slot == view;
				}
				if (!alive)
					return 1;
			}
			view = view->state->parent;
		}
		/* Pasted bytes never reach page shortcuts or raw page handlers. */
		if (event->pasting || event->key == MORPH_TUI_KEY_PASTE_END)
			return rc < 0 ? rc : 1;
	}
	if (!rc && controller->ops->event)
		rc = controller->ops->event(controller, event);
	if (rc || morph_tui_application_transition_pending(app))
		return rc;
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
	/* Resolve focus before binding a paste or incremental text to its target. */
	if (!core_view_can_focus(state->focus)) {
		int rc = core_view_focus_next(controller, 0);

		if (rc)
			return rc;
	}
	if (event->key == MORPH_TUI_KEY_PASTE_BEGIN)
		state->paste_target = state->focus;
	struct morph_tui_view *target = event->pasting ? state->paste_target : state->focus;

	if (target != state->text_target || event->key == MORPH_TUI_KEY_PASTE_BEGIN) {
		morph_terminal_sanitizer_reset(&state->sanitizer);
		state->text_target = target;
	}
	if (event->key >= 32 && event->key < 256 && event->key != 127) {
		char byte = (char)event->key;
		struct morph_tui_event text = *event;

		nb_buf_clear(&state->text);
		int rc = morph_terminal_sanitize_feed(&state->sanitizer, &state->text, &byte, 1, 0);

		if (rc)
			return rc;
		if (!state->text.len)
			return 1;
		text.type = event->pasting ? MORPH_TUI_EVENT_PASTE : MORPH_TUI_EVENT_TEXT;
		text.text = nb_buf_cstr(&state->text);
		text.text_length = state->text.len;
		return core_view_controller_dispatch(data, app, &text);
	}
	int rc = core_view_controller_dispatch(data, app, event);

	if (event->key == MORPH_TUI_KEY_PASTE_END) {
		state->paste_target = NULL;
		morph_terminal_sanitizer_reset(&state->sanitizer);
	}
	return rc;
}

int morph_tui_view_controller_present_borrow(struct morph_tui_application *app,
					     const struct morph_tui_view_controller_spec *spec)
{
	if (!morph_tui_application_is_owner(app))
		return NB_ERR_NOT_PERMITTED;
	struct morph_tui_view_controller *controller = NULL;

	if (!spec || !spec->ops || !spec->ops->build || spec->tick_ms < 0 ||
	    (spec->terminal_flags & ~(MORPH_TUI_TERMINAL_SESSION_ALTERNATE |
		MORPH_TUI_TERMINAL_SESSION_PASTE | MORPH_TUI_TERMINAL_SESSION_MOUSE |
		MORPH_TUI_TERMINAL_SESSION_FOCUS)) ||
	    (spec->size && spec->size < sizeof(*controller)))
		return NB_ERR_INVAL;
	size_t size = spec->size ? spec->size : sizeof(*controller);

	controller = calloc(1, size);
	if (!controller)
		return NB_ERR_NOMEM;
	controller->ops = spec->ops;
	controller->data = spec->data;
	controller->state = calloc(1, sizeof(*controller->state));
	if (!controller->state) {
		free(controller);
		return NB_ERR_NOMEM;
	}
	struct morph_tui_view_controller_state *state = controller->state;

	state->app = app;
	state->quit_key = spec->quit_key;
	state->builder.controller = controller;
	struct morph_tui_view *view;
	int rc = (nb_vec_init(&state->views, sizeof(view)),
		  nb_vec_reserve(&state->views, 16) ? 0 : NB_ERR_NOMEM);

	if (!rc)
		rc = (nb_buf_init(&state->text),
		      nb_buf_reserve(&state->text, 16) ? 0 : NB_ERR_NOMEM);
	morph_terminal_sanitizer_init(&state->sanitizer, MORPH_TERMINAL_TEXT_SINGLE_LINE);

	if (!rc) {
		controller->ops->build(controller, &state->builder);
		rc = state->builder.error;
		if (!rc && !state->root)
			rc = NB_ERR_INVAL;
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
			MORPH_TUI_TERMINAL_SESSION_ALTERNATE | MORPH_TUI_TERMINAL_SESSION_PASTE |
				spec->terminal_flags,
		.tick_ms = spec->tick_ms,
	};

	if (!rc)
		rc = morph_tui_application_present_controller_take(app, &descriptor);
	if (rc)
		core_view_controller_close(controller, rc);
	return rc;
}

int morph_tui_view_controller_dismiss(struct morph_tui_view_controller *controller, int result)
{
	if (!controller || !controller->state->visible)
		return NB_ERR_BUSY;
	return morph_tui_application_dismiss_controller(controller->state->app, result);
}

struct morph_tui_application *
morph_tui_view_controller_application_borrow(const struct morph_tui_view_controller *controller)
{
	return controller ? controller->state->app : NULL;
}
