#ifndef MORPH_TUI_CORE_WINDOW_H
#define MORPH_TUI_CORE_WINDOW_H

#include <morph/tui/core/view.h>
#include <morph/tui/render/backend.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_core_app;
struct morph_tui_core_window;

struct morph_tui_core_window *morph_tui_core_app_window(struct morph_tui_core_app *app);
struct morph_tui_core_view *morph_tui_core_window_root(const struct morph_tui_core_window *window);
struct morph_tui_core_view *morph_tui_core_window_focus(const struct morph_tui_core_window *window);
int morph_tui_core_window_set_backend(struct morph_tui_core_window *window,
				      struct morph_tui_render_backend *backend);
/* Receives input declined by views and their controller, before default page
 * commands. The callback/context are borrowed; NULL removes the handler. */
int morph_tui_core_window_set_event_handler(struct morph_tui_core_window *window,
					    int (*event)(void *,
							 const struct morph_tui_core_event *),
					    void *data);

#ifdef __cplusplus
}
#endif
#endif
