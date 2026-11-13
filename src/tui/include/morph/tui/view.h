#ifndef MORPH_TUI_VIEW_H
#define MORPH_TUI_VIEW_H

#include <morph/tui/event.h>
#include <morph/tui/canvas.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_tui_builder;
struct morph_tui_view;
struct morph_tui_view_state;
struct morph_tui_view_controller;
struct morph_tui_layout;

struct morph_tui_view_size {
	int width, height;
};
struct morph_tui_view_rect {
	int x, y, width, height;
};

enum morph_tui_view_alignment {
	MORPH_TUI_VIEW_STRETCH,
	MORPH_TUI_VIEW_START,
	MORPH_TUI_VIEW_CENTER,
	MORPH_TUI_VIEW_END
};

struct morph_tui_view_insets {
	int top, right, bottom, left;
};

/* Zero means intrinsic size. Positive grow shares remaining stack space.
 * Sizes include padding and exclude margin. Insets are nonnegative cell counts;
 * margins add between siblings and are never painted by the view. */
struct morph_tui_view_options {
	int width, height;
	int min_width, min_height;
	int max_width, max_height;
	int grow;
	struct morph_tui_view_insets margin, padding;
	enum morph_tui_view_alignment alignment;
	/* Inherited by descendants; a background paints the frame, including padding. */
	struct morph_tui_text_style style;
};

/* Embed this base first in custom views. Omitted methods use base behavior.
 * destroy releases subclass resources; the controller frees the instance. */
struct morph_tui_view_ops {
	int (*measure)(struct morph_tui_view *, int width, struct morph_tui_view_size *size);
	int (*layout)(struct morph_tui_view *);
	int (*draw)(struct morph_tui_view *, struct morph_tui_canvas *);
	int (*event)(struct morph_tui_view *, const struct morph_tui_event *);
	void (*destroy)(struct morph_tui_view *);
};

struct morph_tui_view {
	const struct morph_tui_view_ops *ops;
	struct morph_tui_view_state *state;
};

/* Adds a controller-owned node; return value is borrowed until its controller
 * closes or a failed build is rolled back. The caller never frees the node.
 * ops is borrowed for that lifetime; options is copied during this call. */
struct morph_tui_view *morph_tui_view_add_borrow(struct morph_tui_builder *builder,
						 struct morph_tui_view *parent, size_t size,
						 const struct morph_tui_view_ops *ops,
						 const struct morph_tui_view_options *options);
void morph_tui_builder_fail(struct morph_tui_builder *builder, int error);
int morph_tui_builder_error(const struct morph_tui_builder *builder);
/* Borrowed controller/children; valid only while the owning controller lives. */
struct morph_tui_view_controller *morph_tui_view_owner_borrow(const struct morph_tui_view *view);
size_t morph_tui_view_child_count(const struct morph_tui_view *view);
struct morph_tui_view *morph_tui_view_child_borrow(const struct morph_tui_view *view, size_t index);
/* Destroys descendants and invalidates their borrowed handles. The view itself
 * remains attached. Owner thread only; useful for replacing dynamic content. */
int morph_tui_view_clear_children(struct morph_tui_view *);
struct morph_tui_view_rect morph_tui_view_frame(const struct morph_tui_view *view);
/* Local content rectangle after padding; draw receives a canvas of this size. */
struct morph_tui_view_rect morph_tui_view_content_rect(const struct morph_tui_view *view);
/* Apply the box-model inset rules to an arbitrary slot. Margin is outside the
 * returned frame; padding is not applied here. */
struct morph_tui_view_rect morph_tui_view_inset_rect(struct morph_tui_view_rect rect,
						     struct morph_tui_view_insets insets);
struct morph_tui_view_insets morph_tui_view_insets_all(int cells);
struct morph_tui_view_options morph_tui_view_get_options(const struct morph_tui_view *view);
int morph_tui_view_set_margin(struct morph_tui_view *, struct morph_tui_view_insets);
int morph_tui_view_set_padding(struct morph_tui_view *, struct morph_tui_view_insets);
int morph_tui_view_set_style(struct morph_tui_view *, struct morph_tui_text_style);
int morph_tui_view_is_owner(const struct morph_tui_view *view);
/* Measurement includes margins. Arrange accepts that outer allocation and
 * stores a frame inside the margins, including padding. Root views follow
 * the same rules; custom layouts must not apply margins a second time. */
int morph_tui_view_measure(struct morph_tui_view *view, int width,
			   struct morph_tui_view_size *size);
int morph_tui_view_arrange(struct morph_tui_view *view, struct morph_tui_view_rect frame);
void morph_tui_view_invalidate(struct morph_tui_view *view, int layout);
int morph_tui_view_set_hidden(struct morph_tui_view *view, int hidden);
int morph_tui_view_hidden(const struct morph_tui_view *view);
int morph_tui_view_set_focusable(struct morph_tui_view *view, int focusable);
int morph_tui_view_focus(struct morph_tui_view *view);
int morph_tui_view_focused(const struct morph_tui_view *view);
/* Layout managers are borrowed, normally embedded in a custom container. */
int morph_tui_view_set_layout_borrow(struct morph_tui_view *view, struct morph_tui_layout *layout);

#ifdef __cplusplus
}
#endif
#endif
