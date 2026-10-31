#ifndef MORPH_TUI_CORE_VIEW_INTERNAL_H
#define MORPH_TUI_CORE_VIEW_INTERNAL_H

#include <morph/tui/core/view_controller.h>
#include <morph/tui/core/layout.h>
#include <morph/tui/core/window.h>
#include <morph/tui/render/frame.h>
#include "util/arena.h"
#include "util/array.h"
#include "util/utf8.h"

struct morph_tui_core_view_state {
	struct morph_tui_core_view_controller *controller;
	struct morph_tui_core_view *parent;
	morph_array_t children;
	struct morph_tui_core_view_rect frame;
	struct morph_tui_core_view_options options;
	struct morph_tui_core_layout *layout;
	int hidden, focusable, tag;
	int (*action)(void *, const struct morph_tui_widget_action *);
	void *action_data;
};

struct morph_tui_core_builder {
	struct morph_tui_core_view_controller *controller;
	int error;
};

struct morph_tui_core_view_controller_state {
	struct morph_tui_core_app *app;
	struct arena *arena;
	morph_array_t views;
	struct morph_tui_core_builder builder;
	struct morph_tui_core_view *root, *focus;
	struct morph_tui_core_view *text_target, *paste_target;
	struct utf8_terminal_sanitizer sanitizer;
	morph_buf_t text;
	int quit_key, visible;
};

struct morph_tui_core_window {
	struct morph_tui_core_app *app;
	struct morph_tui_core_view_controller *controller;
	struct morph_tui_render_surface *surface;
	struct morph_tui_render_frame frame;
	morph_buf_t rows;
	struct morph_tui_render_backend *backend;
	int (*event)(void *, const struct morph_tui_core_event *);
	void *event_data;
};

int core_view_draw(struct morph_tui_core_view *view, struct morph_tui_render_canvas *parent);
void core_view_scope_destroy(struct morph_tui_core_view_controller *controller);
int core_view_focus_next(struct morph_tui_core_view_controller *controller, int backwards);
int core_view_can_focus(const struct morph_tui_core_view *view);
int core_window_create(struct morph_tui_core_app *app, struct morph_tui_core_window **out);
void core_window_destroy(struct morph_tui_core_window *window);
void core_window_invalidate(struct morph_tui_core_window *window);
int core_window_draw(struct morph_tui_core_window *window,
		     const struct morph_tui_core_screen *screen);

#endif
