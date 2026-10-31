#ifndef MORPH_TUI_CORE_VIEW_H
#define MORPH_TUI_CORE_VIEW_H

#include <morph/tui/core/event.h>
#include <morph/tui/render/canvas.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_core_builder;
struct morph_tui_core_view;
struct morph_tui_core_view_state;
struct morph_tui_core_view_controller;
struct morph_tui_core_layout;

struct morph_tui_core_view_size {
	int width, height;
};
struct morph_tui_core_view_rect {
	int x, y, width, height;
};

enum morph_tui_core_view_alignment {
	MORPH_TUI_CORE_VIEW_STRETCH,
	MORPH_TUI_CORE_VIEW_START,
	MORPH_TUI_CORE_VIEW_CENTER,
	MORPH_TUI_CORE_VIEW_END
};

/* Zero means intrinsic size. Positive grow shares remaining stack space.
 * Padding applies on all sides. All dimensions are terminal cell counts. */
struct morph_tui_core_view_options {
	int width, height;
	int min_width, min_height;
	int max_width, max_height;
	int grow, padding;
	enum morph_tui_core_view_alignment alignment;
};

/* Embed this base first in custom views. Omitted methods use base behavior.
 * destroy releases subclass resources; the controller frees the instance. */
struct morph_tui_core_view_ops {
	int (*measure)(struct morph_tui_core_view *, int width,
		       struct morph_tui_core_view_size *size);
	int (*layout)(struct morph_tui_core_view *);
	int (*draw)(struct morph_tui_core_view *, struct morph_tui_render_canvas *);
	int (*event)(struct morph_tui_core_view *, const struct morph_tui_core_event *);
	void (*destroy)(struct morph_tui_core_view *);
};

struct morph_tui_core_view {
	const struct morph_tui_core_view_ops *ops;
	struct morph_tui_core_view_state *state;
};

struct morph_tui_core_view *
morph_tui_core_view_create(struct morph_tui_core_builder *builder,
			   struct morph_tui_core_view *parent, size_t size,
			   const struct morph_tui_core_view_ops *ops,
			   const struct morph_tui_core_view_options *options);
void morph_tui_core_builder_fail(struct morph_tui_core_builder *builder, int error);
int morph_tui_core_builder_error(const struct morph_tui_core_builder *builder);
struct morph_tui_core_view_controller *
morph_tui_core_view_owner(const struct morph_tui_core_view *view);
size_t morph_tui_core_view_child_count(const struct morph_tui_core_view *view);
struct morph_tui_core_view *morph_tui_core_view_child(const struct morph_tui_core_view *view,
						      size_t index);
struct morph_tui_core_view_rect morph_tui_core_view_frame(const struct morph_tui_core_view *view);
struct morph_tui_core_view_options
morph_tui_core_view_get_options(const struct morph_tui_core_view *view);
int morph_tui_core_view_is_owner(const struct morph_tui_core_view *view);
int morph_tui_core_view_measure(struct morph_tui_core_view *view, int width,
				struct morph_tui_core_view_size *size);
int morph_tui_core_view_arrange(struct morph_tui_core_view *view,
				struct morph_tui_core_view_rect frame);
void morph_tui_core_view_invalidate(struct morph_tui_core_view *view, int layout);
int morph_tui_core_view_set_hidden(struct morph_tui_core_view *view, int hidden);
int morph_tui_core_view_hidden(const struct morph_tui_core_view *view);
int morph_tui_core_view_set_focusable(struct morph_tui_core_view *view, int focusable);
int morph_tui_core_view_focus(struct morph_tui_core_view *view);
int morph_tui_core_view_focused(const struct morph_tui_core_view *view);
/* Layout managers are borrowed, normally embedded in a custom container. */
int morph_tui_core_view_set_layout(struct morph_tui_core_view *view,
				   struct morph_tui_core_layout *layout);

#ifdef __cplusplus
}
#endif
#endif
