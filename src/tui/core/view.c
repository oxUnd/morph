#include "view_internal.h"
#include "util/error.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

void morph_tui_builder_fail(struct morph_tui_builder *builder, int error)
{
	if (builder && error < 0 && !builder->error)
		builder->error = error;
}

int morph_tui_builder_error(const struct morph_tui_builder *builder)
{
	return builder ? builder->error : -EINVAL;
}

static int core_view_insets_valid(struct morph_tui_view_insets insets)
{
	return insets.top >= 0 && insets.right >= 0 && insets.bottom >= 0 && insets.left >= 0 &&
	       insets.left <= INT_MAX - insets.right && insets.top <= INT_MAX - insets.bottom;
}

static int core_view_options_valid(const struct morph_tui_view_options *options)
{
	if (!morph_tui_text_style_valid(options->style))
		return 0;
	if (!core_view_insets_valid(options->margin) || !core_view_insets_valid(options->padding))
		return 0;
	int horizontal = options->margin.left + options->margin.right;
	int vertical = options->margin.top + options->margin.bottom;

	return options->width >= 0 && options->height >= 0 && options->min_width >= 0 &&
	       options->min_height >= 0 && options->max_width >= 0 && options->max_height >= 0 &&
	       options->grow >= 0 && options->width <= INT_MAX - horizontal &&
	       options->min_width <= INT_MAX - horizontal &&
	       options->max_width <= INT_MAX - horizontal &&
	       options->height <= INT_MAX - vertical && options->min_height <= INT_MAX - vertical &&
	       options->max_height <= INT_MAX - vertical &&
	       (!options->max_width || options->max_width >= options->min_width) &&
	       (!options->max_height || options->max_height >= options->min_height) &&
	       options->alignment >= MORPH_TUI_VIEW_STRETCH &&
	       options->alignment <= MORPH_TUI_VIEW_END;
}

struct morph_tui_view *morph_tui_view_add_borrow(struct morph_tui_builder *builder,
						 struct morph_tui_view *parent, size_t size,
						 const struct morph_tui_view_ops *ops,
						 const struct morph_tui_view_options *options)
{
	if (!builder || builder->error)
		return NULL;
	struct morph_tui_view_controller_state *scope = builder->controller->state;
	struct morph_tui_view_options opts =
		options ? *options : (struct morph_tui_view_options){0};

	if (!morph_tui_application_is_owner(scope->app)) {
		morph_tui_builder_fail(builder, -EPERM);
		return NULL;
	}
	if (size < sizeof(*parent) || !core_view_options_valid(&opts) ||
	    (parent && (!parent->state || parent->state->controller != builder->controller)) ||
	    (!parent && scope->root)) {
		morph_tui_builder_fail(builder, -EINVAL);
		return NULL;
	}
	struct morph_tui_view *view = calloc(1, size);
	struct morph_tui_view_state *state = calloc(1, sizeof(*state));

	if (!view || !state) {
		free(state);
		free(view);
		morph_tui_builder_fail(builder, -ENOMEM);
		return NULL;
	}
	view->ops = ops;
	view->state = state;
	state->controller = builder->controller;
	state->parent = parent;
	state->options = opts;
	int rc = morph_array_init(&state->children, 4, sizeof(view));
	struct morph_tui_view **slot = rc ? NULL : morph_array_push_borrow(&scope->views);

	if (!slot) {
		morph_array_cleanup(&state->children);
		free(state);
		free(view);
		morph_tui_builder_fail(builder, rc ? rc : -ENOMEM);
		return NULL;
	}
	*slot = view;
	if (parent) {
		slot = morph_array_push_borrow(&parent->state->children);
		if (slot)
			*slot = view;
		else
			morph_tui_builder_fail(builder, -ENOMEM);
	} else {
		scope->root = view;
	}
	if (!builder->error)
		morph_tui_view_invalidate(view, 1);
	scope->structure_revision++;
	return view;
}

struct morph_tui_view_controller *morph_tui_view_owner_borrow(const struct morph_tui_view *view)
{
	return view && view->state ? view->state->controller : NULL;
}

size_t morph_tui_view_child_count(const struct morph_tui_view *view)
{
	return view && view->state ? view->state->children.nelts : 0;
}

struct morph_tui_view *morph_tui_view_child_borrow(const struct morph_tui_view *view, size_t index)
{
	struct morph_tui_view **slot =
		view ? morph_array_borrow(&view->state->children, index) : NULL;

	return slot ? *slot : NULL;
}

int morph_tui_view_clear_children(struct morph_tui_view *view)
{
	if (!view)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct morph_tui_view_controller_state *scope = view->state->controller->state;
	for (size_t i = scope->views.nelts; i > 0; i--) {
		struct morph_tui_view **slot = morph_array_borrow(&scope->views, i - 1);
		struct morph_tui_view *node = *slot;
		struct morph_tui_view *ancestor = node->state->parent;
		while (ancestor && ancestor != view)
			ancestor = ancestor->state->parent;
		if (!ancestor)
			continue;
		if (scope->focus == node)
			scope->focus = NULL;
		if (scope->text_target == node)
			scope->text_target = NULL;
		if (scope->paste_target == node)
			scope->paste_target = NULL;
		if (scope->mouse_target == node)
			scope->mouse_target = NULL;
		if (node->ops && node->ops->destroy)
			node->ops->destroy(node);
		morph_array_cleanup(&node->state->children);
		free(node->state);
		free(node);
		memmove(slot, slot + 1, (scope->views.nelts - i) * sizeof(*slot));
		morph_array_pop(&scope->views);
	}
	morph_array_clear(&view->state->children);
	scope->structure_revision++;
	morph_tui_view_invalidate(view, 1);
	return 0;
}

struct morph_tui_view_rect morph_tui_view_frame(const struct morph_tui_view *view)
{
	return view ? view->state->frame : (struct morph_tui_view_rect){0};
}

struct morph_tui_view_insets morph_tui_view_insets_all(int cells)
{
	return (struct morph_tui_view_insets){cells, cells, cells, cells};
}

static struct morph_tui_view_rect core_view_inset(struct morph_tui_view_rect rect,
						  struct morph_tui_view_insets insets)
{
	int horizontal = insets.left + insets.right;
	int vertical = insets.top + insets.bottom;

	rect.x += insets.left < rect.width ? insets.left : rect.width;
	rect.y += insets.top < rect.height ? insets.top : rect.height;
	rect.width = rect.width > horizontal ? rect.width - horizontal : 0;
	rect.height = rect.height > vertical ? rect.height - vertical : 0;
	return rect;
}

struct morph_tui_view_rect morph_tui_view_content_rect(const struct morph_tui_view *view)
{
	if (!view)
		return (struct morph_tui_view_rect){0};
	struct morph_tui_view_rect frame = view->state->frame;

	frame.x = 0;
	frame.y = 0;
	return core_view_inset(frame, view->state->options.padding);
}

struct morph_tui_view_rect morph_tui_view_inset_rect(struct morph_tui_view_rect rect,
						     struct morph_tui_view_insets insets)
{
	return core_view_inset(rect, insets);
}

struct morph_tui_view_options morph_tui_view_get_options(const struct morph_tui_view *view)
{
	return view ? view->state->options : (struct morph_tui_view_options){0};
}

static int core_view_set_insets(struct morph_tui_view *view, struct morph_tui_view_insets insets,
				int padding)
{
	if (!view)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct morph_tui_view_options opts = view->state->options;
	struct morph_tui_view_insets old = padding ? opts.padding : opts.margin;

	if (padding)
		opts.padding = insets;
	else
		opts.margin = insets;
	if (!core_view_options_valid(&opts))
		return -EINVAL;
	if (old.top == insets.top && old.right == insets.right && old.bottom == insets.bottom &&
	    old.left == insets.left)
		return 0;
	view->state->options = opts;
	morph_tui_view_invalidate(view, 1);
	return 0;
}

int morph_tui_view_set_margin(struct morph_tui_view *view, struct morph_tui_view_insets insets)
{
	return core_view_set_insets(view, insets, 0);
}

int morph_tui_view_set_padding(struct morph_tui_view *view, struct morph_tui_view_insets insets)
{
	return core_view_set_insets(view, insets, 1);
}

int morph_tui_view_set_style(struct morph_tui_view *view, struct morph_tui_text_style style)
{
	if (!view || !morph_tui_text_style_valid(style))
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	if (!morph_tui_text_style_equal(view->state->options.style, style)) {
		view->state->options.style = style;
		morph_tui_view_invalidate(view, 0);
	}
	return 0;
}

int morph_tui_view_is_owner(const struct morph_tui_view *view)
{
	return view && view->state &&
	       morph_tui_application_is_owner(view->state->controller->state->app);
}

static int core_view_extent(int value, int minimum, int maximum)
{
	if (value < minimum)
		value = minimum;
	if (maximum && value > maximum)
		value = maximum;
	return value;
}

int morph_tui_view_measure(struct morph_tui_view *view, int width, struct morph_tui_view_size *size)
{
	if (!view || !size || width < 0)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	*size = (struct morph_tui_view_size){0};
	if (view->state->hidden)
		return 0;
	struct morph_tui_view_options opts = view->state->options;
	int margin_width = opts.margin.left + opts.margin.right;
	int margin_height = opts.margin.top + opts.margin.bottom;
	int padding_width = opts.padding.left + opts.padding.right;
	int padding_height = opts.padding.top + opts.padding.bottom;
	int inner = width > margin_width ? width - margin_width : 0;

	inner = inner > padding_width ? inner - padding_width : 0;
	struct morph_tui_layout *manager = view->state->layout;
	int rc = 0;

	if (manager && manager->ops && manager->ops->measure)
		rc = manager->ops->measure(manager, view, inner, size);
	else if (view->ops && view->ops->measure)
		rc = view->ops->measure(view, inner, size);
	if (rc)
		return rc;
	if (size->width < 0 || size->height < 0 || size->width > INT_MAX - padding_width ||
	    size->height > INT_MAX - padding_height)
		return -EOVERFLOW;
	size->width += padding_width;
	size->height += padding_height;
	if (opts.width)
		size->width = opts.width;
	if (opts.height)
		size->height = opts.height;
	size->width = core_view_extent(size->width, opts.min_width, opts.max_width);
	size->height = core_view_extent(size->height, opts.min_height, opts.max_height);
	if (size->width > INT_MAX - margin_width || size->height > INT_MAX - margin_height)
		return -EOVERFLOW;
	size->width += margin_width;
	size->height += margin_height;
	return 0;
}

int morph_tui_view_arrange(struct morph_tui_view *view, struct morph_tui_view_rect frame)
{
	if (!view || frame.x < 0 || frame.y < 0 || frame.width < 0 || frame.height < 0)
		return -EINVAL;
	if (frame.x > INT_MAX - frame.width || frame.y > INT_MAX - frame.height)
		return -EOVERFLOW;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	view->state->frame = core_view_inset(frame, view->state->options.margin);
	struct morph_tui_layout *manager = view->state->layout;

	if (manager && manager->ops && manager->ops->arrange)
		return manager->ops->arrange(manager, view);
	if (view->ops && view->ops->layout)
		return view->ops->layout(view);
	return 0;
}

void morph_tui_view_invalidate(struct morph_tui_view *view, int layout)
{
	if (!view)
		return;
	morph_tui_application_invalidate(view->state->controller->state->app,
					 MORPH_TUI_INVALIDATE_DISPLAY |
						 (layout ? MORPH_TUI_INVALIDATE_LAYOUT : 0));
}

int morph_tui_view_hidden(const struct morph_tui_view *view)
{
	return !view || view->state->hidden;
}

int morph_tui_view_set_hidden(struct morph_tui_view *view, int hidden)
{
	if (!view)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	if (view->state->hidden == !!hidden)
		return 0;
	view->state->hidden = !!hidden;
	if (!core_view_can_focus(view->state->controller->state->focus))
		view->state->controller->state->focus = NULL;
	morph_tui_view_invalidate(view, 1);
	return 0;
}

int core_view_can_focus(const struct morph_tui_view *view)
{
	if (!view || !view->state->focusable)
		return 0;
	for (const struct morph_tui_view *ancestor = view; ancestor;
	     ancestor = ancestor->state->parent)
		if (ancestor->state->hidden)
			return 0;
	return 1;
}

int morph_tui_view_set_focusable(struct morph_tui_view *view, int focusable)
{
	if (!view)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	if (view->state->focusable == !!focusable)
		return 0;
	view->state->focusable = !!focusable;
	if (!focusable && morph_tui_view_focused(view))
		view->state->controller->state->focus = NULL;
	morph_tui_view_invalidate(view, 0);
	return 0;
}

int morph_tui_view_focus(struct morph_tui_view *view)
{
	if (!core_view_can_focus(view))
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	if (morph_tui_view_focused(view))
		return 0;
	view->state->controller->state->focus = view;
	morph_tui_view_invalidate(view, 0);
	return 0;
}

int morph_tui_view_focused(const struct morph_tui_view *view)
{
	return view && view->state->controller->state->focus == view;
}

int core_view_focus_next(struct morph_tui_view_controller *controller, int backwards)
{
	struct morph_tui_view_controller_state *scope = controller->state;
	struct morph_tui_view *view = scope->root, *previous = NULL, *first = NULL;
	struct morph_tui_view *after = NULL;
	int found = scope->focus == NULL;

	/* Walk the actual tree, including children attached after later siblings. */
	for (size_t n = 0; view && n < scope->views.nelts; n++) {
		if (core_view_can_focus(view)) {
			if (!first)
				first = view;
			if (found && !after && view != scope->focus)
				after = view;
			if (view == scope->focus) {
				if (backwards && previous)
					return morph_tui_view_focus(previous);
				found = 1;
			}
			previous = view;
		}
		if (morph_tui_view_child_count(view)) {
			view = morph_tui_view_child_borrow(view, 0);
			continue;
		}
		struct morph_tui_view *next = NULL;

		while (view && view->state->parent && !next) {
			struct morph_tui_view *parent = view->state->parent;

			for (size_t i = 0; i + 1 < morph_tui_view_child_count(parent); i++)
				if (morph_tui_view_child_borrow(parent, i) == view) {
					next = morph_tui_view_child_borrow(parent, i + 1);
					break;
				}
			view = parent;
		}
		view = next;
	}
	struct morph_tui_view *target = backwards ? previous : (after ? after : first);

	if (target)
		return morph_tui_view_focus(target);
	return 0;
}

int morph_tui_view_set_layout_borrow(struct morph_tui_view *view, struct morph_tui_layout *layout)
{
	if (!view || (layout && !layout->ops))
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	view->state->layout = layout;
	morph_tui_view_invalidate(view, 1);
	return 0;
}

static int core_view_draw_padding(struct morph_tui_view *view,
				  struct morph_tui_canvas *frame,
				  struct morph_tui_view_rect content)
{
	if (!view->ops || !view->ops->draw_padding)
		return 0;
	const struct morph_tui_view_rect edges[] = {
		{0, 0, frame->width, content.y},
		{content.x + content.width, content.y,
		 frame->width - content.x - content.width, content.height},
		{0, content.y + content.height, frame->width,
		 frame->height - content.y - content.height},
		{0, content.y, content.x, content.height},
	};
	for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
		struct morph_tui_canvas edge;
		morph_tui_canvas_child_borrow(frame, edges[i].x, edges[i].y,
			edges[i].width, edges[i].height, &edge);
		if (edge.left >= edge.right || edge.top >= edge.bottom)
			continue;
		int rc = view->ops->draw_padding(view, (enum morph_tui_view_edge)i, &edge);
		if (rc)
			return rc;
	}
	return 0;
}

int core_view_draw(struct morph_tui_view *view, struct morph_tui_canvas *parent)
{
	if (view->state->hidden)
		return 0;
	struct morph_tui_view_rect frame = view->state->frame;
	struct morph_tui_canvas canvas;

	morph_tui_canvas_child_borrow(parent, frame.x, frame.y, frame.width, frame.height, &canvas);
	if (canvas.left >= canvas.right || canvas.top >= canvas.bottom)
		return 0;
	struct morph_tui_text_style style = view->state->options.style;
	canvas.style.attributes |= style.attributes;
	if (style.foreground.kind)
		canvas.style.foreground = style.foreground;
	if (style.background.kind) {
		canvas.style.background = style.background;
		int fill_rc = morph_tui_canvas_fill(&canvas, 0);
		if (fill_rc)
			return fill_rc;
	}
	struct morph_tui_view_rect rect = morph_tui_view_content_rect(view);
	struct morph_tui_canvas content;

	morph_tui_canvas_child_borrow(&canvas, rect.x, rect.y, rect.width, rect.height, &content);
	int rc = core_view_draw_padding(view, &canvas, rect);
	if (!rc && view->ops && view->ops->draw)
		rc = view->ops->draw(view, &content);

	struct morph_tui_canvas children = canvas;
	children.left = content.left;
	children.top = content.top;
	children.right = content.right;
	children.bottom = content.bottom;
	children.y -= view->state->scroll_y;
	for (size_t i = 0; i < view->state->children.nelts && !rc; i++)
		rc = core_view_draw(morph_tui_view_child_borrow(view, i), &children);
	return rc;
}

void core_view_scope_destroy(struct morph_tui_view_controller *controller)
{
	for (size_t i = controller->state->views.nelts; i > 0; i--) {
		struct morph_tui_view **slot = morph_array_borrow(&controller->state->views, i - 1);

		if ((*slot)->ops && (*slot)->ops->destroy)
			(*slot)->ops->destroy(*slot);
		morph_array_cleanup(&(*slot)->state->children);
		free((*slot)->state);
		free(*slot);
	}
	morph_array_cleanup(&controller->state->views);
}
