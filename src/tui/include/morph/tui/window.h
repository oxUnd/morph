#ifndef MORPH_TUI_WINDOW_H
#define MORPH_TUI_WINDOW_H

#include <morph/tui/view.h>
#include <morph/tui/renderer.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_application;
struct morph_tui_window;

/* Application owns its window; the result lasts until application deinit. */
struct morph_tui_window *morph_tui_application_window_borrow(struct morph_tui_application *app);
/* Root/focus are borrowed until their controller closes or focus changes. */
struct morph_tui_view *morph_tui_window_root_borrow(const struct morph_tui_window *window);
struct morph_tui_view *morph_tui_window_focus_borrow(const struct morph_tui_window *window);
/* Borrows backend through replacement or application deinit. */
int morph_tui_window_set_renderer_borrow(struct morph_tui_window *window,
					 struct morph_tui_renderer *backend);
/* Receives input declined by views and their controller, before default page
 * commands. The callback/context are borrowed; NULL removes the handler. */
int morph_tui_window_set_event_handler_borrow(struct morph_tui_window *window,
					      int (*event)(void *, const struct morph_tui_event *),
					      void *data);

#ifdef __cplusplus
}
#endif
#endif
