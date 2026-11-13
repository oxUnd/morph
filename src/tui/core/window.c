#include "view_internal.h"
#include "errors.h"
#include <morph/tui/draw.h>
#include "tui/render/native_media.h"
#include "tui/render/frame_internal.h"
#include "nb/cleanup.h"

#include <stdlib.h>
#include <string.h>

int core_window_create(struct morph_tui_application *app, struct morph_tui_window **out)
{
	if (!out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	struct morph_tui_window *window = calloc(1, sizeof(*window));

	if (!window)
		return NB_ERR_NOMEM;
	window->app = app;
	int rc = morph_tui_surface_create(&window->surface);

	if (!rc)
		rc = (nb_buf_init(&window->rows),
		      nb_buf_reserve(&window->rows, BUFSIZ) ? 0 : NB_ERR_NOMEM);
	if (rc) {
		core_window_destroy(window);
		return rc;
	}
	*out = window;
	return 0;
}

void core_window_destroy(struct morph_tui_window *window)
{
	if (!window)
		return;
	core_window_clear_media(window);
	morph_tui_frame_cleanup(&window->frame);
	morph_tui_surface_destroy(window->surface);
	nb_buf_cleanup(&window->rows);
	nb_buf_cleanup(&window->selection_text);
	free(window);
}

void core_window_clear_media(struct morph_tui_window *window)
{
	if (!window)
		return;
	render_native_media_destroy(window->media);
	window->media = NULL;
}

void core_window_invalidate(struct morph_tui_window *window)
{
	morph_tui_frame_invalidate(&window->frame);
	render_native_media_invalidate(window->media);
	if (window->backend && window->backend->ops->invalidate)
		window->backend->ops->invalidate(window->backend);
}

struct morph_tui_view *morph_tui_window_root_borrow(const struct morph_tui_window *window)
{
	return window && window->controller ? window->controller->state->root : NULL;
}

struct morph_tui_view *morph_tui_window_focus_borrow(const struct morph_tui_window *window)
{
	return window && window->controller ? window->controller->state->focus : NULL;
}

int morph_tui_window_set_renderer_borrow(struct morph_tui_window *window,
					 struct morph_tui_renderer *backend)
{
	if (!window || (backend && (!backend->ops || !backend->ops->present)))
		return NB_ERR_INVAL;
	if (!morph_tui_application_is_owner(window->app))
		return NB_ERR_NOT_PERMITTED;
	if (window->backend != backend)
		core_window_clear_media(window);
	window->backend = backend;
	core_window_invalidate(window);
	morph_tui_application_invalidate(window->app, MORPH_TUI_INVALIDATE_DISPLAY);
	return 0;
}

int morph_tui_window_set_event_handler_borrow(struct morph_tui_window *window,
					      int (*event)(void *, const struct morph_tui_event *),
					      void *data)
{
	if (!window)
		return NB_ERR_INVAL;
	if (!morph_tui_application_is_owner(window->app))
		return NB_ERR_NOT_PERMITTED;
	window->event = event;
	window->event_data = data;
	return 0;
}

void core_window_clear_selection(struct morph_tui_window *window)
{
	if (!window)
		return;
	int changed = window->selection_active;

	window->selection_active = 0;
	window->selection_dragging = 0;
	nb_buf_clear(&window->selection_text);
	if (changed)
		morph_tui_application_invalidate(window->app, MORPH_TUI_INVALIDATE_DISPLAY);
}

int morph_tui_window_set_text_copy_handler_borrow(struct morph_tui_window *window,
						 int (*copy)(void *, const char *), void *data)
{
	if (!window)
		return NB_ERR_INVAL;
	if (!morph_tui_application_is_owner(window->app))
		return NB_ERR_NOT_PERMITTED;
	core_window_clear_selection(window);
	window->copy_text = copy;
	window->copy_data = data;
	return 0;
}

int core_window_selection_dragging(const struct morph_tui_window *window)
{
	return window && window->selection_dragging;
}

static int core_window_selection_capture(struct morph_tui_window *window)
{
	return morph_tui_surface_selection_text(window->surface, window->selection_x1,
		window->selection_y1, window->selection_x2, window->selection_y2,
		&window->selection_text);
}

int core_window_mouse_event(struct morph_tui_window *window, const struct morph_tui_event *event)
{
	if (!window || !event)
		return NB_ERR_INVAL;
	if (event->type != MORPH_TUI_EVENT_MOUSE || !window->copy_text ||
	    event->mouse.modifiers)
		return 0;
	const struct morph_tui_mouse_event *mouse = &event->mouse;
	struct morph_tui_canvas canvas;

	morph_tui_canvas_root_borrow(window->surface, &canvas);
	if (!canvas.width || !canvas.height)
		return 0;
	if (mouse->button == MORPH_TUI_MOUSE_RIGHT && mouse->action == MORPH_TUI_MOUSE_PRESS) {
		if (!window->selection_active || !window->selection_text.len)
			return 0;
		int rc = window->copy_text(window->copy_data, nb_buf_cstr(&window->selection_text));

		return rc < 0 ? rc : 1;
	}
	if (mouse->button != MORPH_TUI_MOUSE_LEFT)
		return 0;
	if (mouse->action == MORPH_TUI_MOUSE_PRESS) {
		if (mouse->x < 0 || mouse->y < 0 || mouse->x >= canvas.width ||
		    mouse->y >= canvas.height)
			return 0;
		core_window_clear_selection(window);
		window->selection_dragging = 1;
		window->selection_x1 = window->selection_x2 = mouse->x;
		window->selection_y1 = window->selection_y2 = mouse->y;
		window->selection_width = canvas.width;
		window->selection_height = canvas.height;
		return 1;
	}
	if (!window->selection_dragging)
		return 0;
	window->selection_x2 = mouse->x;
	window->selection_y2 = mouse->y;
	window->selection_active |= window->selection_x1 != mouse->x ||
		window->selection_y1 != mouse->y;
	if (mouse->action == MORPH_TUI_MOUSE_RELEASE)
		window->selection_dragging = 0;
	int rc = window->selection_active ? core_window_selection_capture(window) : 0;

	if (rc)
		core_window_clear_selection(window);
	morph_tui_application_invalidate(window->app, MORPH_TUI_INVALIDATE_DISPLAY);
	return rc ? rc : 1;
}

static int core_window_selection_draw(struct morph_tui_window *window,
				       const struct morph_tui_screen *screen)
{
	if (window->selection_width != screen->area.width ||
	    window->selection_height != screen->area.height) {
		core_window_clear_selection(window);
		return 0;
	}
	if (!window->selection_active)
		return 0;
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = morph_tui_surface_selection_text(window->surface, window->selection_x1,
		window->selection_y1, window->selection_x2, window->selection_y2, &text);

	if (rc)
		return rc;
	if (strcmp(nb_buf_cstr(&text), nb_buf_cstr(&window->selection_text))) {
		core_window_clear_selection(window);
		return 0;
	}
	return morph_tui_surface_selection_style(window->surface, window->selection_x1,
		window->selection_y1, window->selection_x2, window->selection_y2,
		MORPH_TUI_STYLE_SELECTED);
}

int core_window_draw(struct morph_tui_window *window, const struct morph_tui_screen *screen)
{
	nb_buf overlay NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = morph_tui_surface_resize(window->surface, screen->area.width, screen->area.height);
	struct morph_tui_canvas canvas;

	if (rc)
		return rc;
	morph_tui_canvas_root_borrow(window->surface, &canvas);
	rc = core_view_draw(window->controller->state->root, &canvas);
	if (!rc)
		rc = core_window_selection_draw(window, screen);
	if (!rc && window->backend)
		return window->backend->ops->present(window->backend, window->surface,
						     screen->session.output);
	if (!rc)
		rc = morph_tui_surface_rows(window->surface, &window->rows);
	if (!rc)
		rc = render_native_media_prepare(&window->media, window->surface, &window->frame);
	if (!rc && render_native_media_needs_refresh(window->media, window->surface, &window->frame,
						     nb_buf_cstr(&window->rows)))
		rc = render_native_media_append(window->media, window->surface,
						screen->session.output, &overlay);
	if (!rc) {
		int x, y;
		int visible = morph_tui_surface_cursor(window->surface, &x, &y);
		if (visible)
			rc = (nb_buf_printf(&overlay, "\033[%d;%dH\033[?25h", y + 1, x + 1)
				      ? 0
				      : NB_ERR_NOMEM);
		else
			rc = (nb_buf_append_cstr(&overlay, "\033[?25l") ? 0 : NB_ERR_NOMEM);
	}
	if (!rc)
		rc = render_frame_present_composed(&window->frame, screen->session.output,
						   screen->area.width, screen->area.height,
						   nb_buf_cstr(&window->rows), &overlay);
	if (rc)
		render_native_media_invalidate(window->media);
	return rc;
}
