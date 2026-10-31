#include "view_internal.h"
#include "util/error.h"
#include <morph/tui/draw.h>

#include <stdlib.h>

int core_window_create(struct morph_tui_application *app, struct morph_tui_window **out)
{
	struct morph_tui_window *window = calloc(1, sizeof(*window));

	if (!window)
		return -ENOMEM;
	window->app = app;
	int rc = morph_tui_surface_create(&window->surface);

	if (!rc)
		rc = morph_buf_init(&window->rows, BUFSIZ);
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
	morph_tui_frame_cleanup(&window->frame);
	morph_tui_surface_destroy(window->surface);
	morph_buf_cleanup(&window->rows);
	free(window);
}

void core_window_invalidate(struct morph_tui_window *window)
{
	morph_tui_frame_invalidate(&window->frame);
	if (window->backend && window->backend->ops->invalidate)
		window->backend->ops->invalidate(window->backend);
}

struct morph_tui_view *morph_tui_window_root(const struct morph_tui_window *window)
{
	return window && window->controller ? window->controller->state->root : NULL;
}

struct morph_tui_view *morph_tui_window_focus(const struct morph_tui_window *window)
{
	return window && window->controller ? window->controller->state->focus : NULL;
}

int morph_tui_window_set_renderer(struct morph_tui_window *window,
				  struct morph_tui_renderer *backend)
{
	if (!window || (backend && (!backend->ops || !backend->ops->present)))
		return -EINVAL;
	if (!morph_tui_application_is_owner(window->app))
		return -EPERM;
	window->backend = backend;
	core_window_invalidate(window);
	morph_tui_application_invalidate(window->app, MORPH_TUI_INVALIDATE_DISPLAY);
	return 0;
}

int morph_tui_window_set_event_handler(struct morph_tui_window *window,
				       int (*event)(void *, const struct morph_tui_event *),
				       void *data)
{
	if (!window)
		return -EINVAL;
	if (!morph_tui_application_is_owner(window->app))
		return -EPERM;
	window->event = event;
	window->event_data = data;
	return 0;
}

int core_window_draw(struct morph_tui_window *window, const struct morph_tui_screen *screen)
{
	int rc = morph_tui_surface_resize(window->surface, screen->area.width, screen->area.height);
	struct morph_tui_canvas canvas;

	if (rc)
		return rc;
	morph_tui_canvas_root(window->surface, &canvas);
	rc = core_view_draw(window->controller->state->root, &canvas);
	if (!rc && window->backend)
		return window->backend->ops->present(window->backend, window->surface,
						     screen->session.output);
	if (!rc)
		rc = morph_tui_surface_rows(window->surface, &window->rows);
	if (!rc)
		rc = morph_tui_frame_present(&window->frame, screen->session.output,
					     screen->area.width, screen->area.height,
					     morph_buf_cstr(&window->rows));
	if (!rc) {
		int x, y;
		int visible = morph_tui_surface_cursor(window->surface, &x, &y);

		morph_buf_reset(&window->rows);
		if (visible)
			morph_buf_printf(&window->rows, "\033[%d;%dH\033[?25h", y + 1, x + 1);
		else
			morph_buf_puts(&window->rows, "\033[?25l");
		rc = morph_tui_draw_write(screen->session.output, &window->rows);
	}
	return rc;
}
