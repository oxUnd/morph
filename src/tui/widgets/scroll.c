#include <morph/tui/widgets/scroll.h>
#include <morph/tui/widgets/list.h>
#include "tui/core/view_internal.h"
#include "util/error.h"

struct widget_scroll {
	struct morph_tui_widget base;
	int follow, extent, page;
};

static int widget_scroll_measure(struct morph_tui_view *view, int width,
				 struct morph_tui_view_size *size)
{
	(void)view;
	*size = (struct morph_tui_view_size){width, 1};
	return 0;
}

static int widget_scroll_layout(struct morph_tui_view *view)
{
	struct widget_scroll *scroll = (struct widget_scroll *)view;
	struct morph_tui_view_rect content = morph_tui_view_content_rect(view);
	size_t count = morph_tui_view_child_count(view);
	if (count > 1)
		return -EINVAL;
	scroll->page = content.height;
	scroll->extent = 0;
	if (count) {
		struct morph_tui_view *child = morph_tui_view_child_borrow(view, 0);
		struct morph_tui_view_size size;
		int rc = morph_tui_view_measure(child, content.width, &size);
		if (rc)
			return rc;
		scroll->extent = size.height;
		content.height = size.height > content.height ? size.height : content.height;
		rc = morph_tui_view_arrange(child, content);
		if (rc)
			return rc;
	}
	int maximum = scroll->extent > scroll->page ? scroll->extent - scroll->page : 0;
	if (scroll->follow || view->state->scroll_y > maximum)
		view->state->scroll_y = maximum;
	return 0;
}

static int widget_scroll_event(struct morph_tui_view *view, const struct morph_tui_event *event)
{
	if (event->type != MORPH_TUI_EVENT_KEY || event->pasting)
		return 0;
	return morph_tui_widget_scroll_handle_key(view, event->key);
}

static const struct morph_tui_view_ops widget_scroll_ops = {
	.measure = widget_scroll_measure,
	.layout = widget_scroll_layout,
	.event = widget_scroll_event,
};

int morph_tui_widget_scroll_handle_key(struct morph_tui_view *view, int key)
{
	if (!view || view->ops != &widget_scroll_ops)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	if (key != MORPH_TUI_KEY_PAGE_UP && key != MORPH_TUI_KEY_PAGE_DOWN &&
	    key != MORPH_TUI_KEY_UP && key != MORPH_TUI_KEY_DOWN && key != MORPH_TUI_KEY_HOME &&
	    key != MORPH_TUI_KEY_END)
		return 0;
	struct widget_scroll *scroll = (struct widget_scroll *)view;
	size_t offset = morph_tui_widget_scroll_update(
		(size_t)view->state->scroll_y, (size_t)scroll->extent,
		scroll->page > 0 ? (size_t)scroll->page : 1u, key);
	view->state->scroll_y = (int)offset;
	scroll->follow = view->state->scroll_y >= scroll->extent - scroll->page;
	morph_tui_view_invalidate(view, 0);
	return 1;
}

int morph_tui_widget_scroll_follow(struct morph_tui_view *view, int enabled)
{
	if (!view || view->ops != &widget_scroll_ops)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	((struct widget_scroll *)view)->follow = !!enabled;
	morph_tui_view_invalidate(view, 1);
	return 0;
}

int morph_tui_widget_scroll_offset(const struct morph_tui_view *view)
{
	return view && view->ops == &widget_scroll_ops ? view->state->scroll_y : 0;
}

struct morph_tui_view *
morph_tui_widget_scroll_add_borrow(struct morph_tui_builder *builder, struct morph_tui_view *parent,
				   const struct morph_tui_view_options *options)
{
	struct widget_scroll *scroll = (struct widget_scroll *)morph_tui_widget_add_borrow(
		builder, parent, sizeof(*scroll), &widget_scroll_ops, options);
	if (!scroll)
		return NULL;
	scroll->follow = 1;
	return &scroll->base.view;
}
