#include "view_internal.h"
#include "util/error.h"

#include <limits.h>
#include <string.h>

void morph_tui_core_builder_fail(struct morph_tui_core_builder *builder, int error)
{
	if (builder && error < 0 && !builder->error)
		builder->error = error;
}

int morph_tui_core_builder_error(const struct morph_tui_core_builder *builder)
{
	return builder ? builder->error : -EINVAL;
}

static int core_view_options_valid(const struct morph_tui_core_view_options *options)
{
	return options->width >= 0 && options->height >= 0 && options->min_width >= 0 &&
	       options->min_height >= 0 && options->max_width >= 0 && options->max_height >= 0 &&
	       options->grow >= 0 && options->padding >= 0 && options->padding <= INT_MAX / 2 &&
	       (!options->max_width || options->max_width >= options->min_width) &&
	       (!options->max_height || options->max_height >= options->min_height) &&
	       options->alignment >= MORPH_TUI_CORE_VIEW_STRETCH &&
	       options->alignment <= MORPH_TUI_CORE_VIEW_END;
}

struct morph_tui_core_view *
morph_tui_core_view_create(struct morph_tui_core_builder *builder,
			   struct morph_tui_core_view *parent, size_t size,
			   const struct morph_tui_core_view_ops *ops,
			   const struct morph_tui_core_view_options *options)
{
	if (!builder || builder->error)
		return NULL;
	struct morph_tui_core_view_controller_state *scope = builder->controller->state;
	struct morph_tui_core_view_options opts =
		options ? *options : (struct morph_tui_core_view_options){0};

	if (!morph_tui_core_app_is_owner(scope->app)) {
		morph_tui_core_builder_fail(builder, -EPERM);
		return NULL;
	}
	if (size < sizeof(*parent) || !core_view_options_valid(&opts) ||
	    (parent && (!parent->state || parent->state->controller != builder->controller)) ||
	    (!parent && scope->root)) {
		morph_tui_core_builder_fail(builder, -EINVAL);
		return NULL;
	}
	struct morph_tui_core_view *view = arena_alloc(scope->arena, size);
	struct morph_tui_core_view_state *state = arena_alloc(scope->arena, sizeof(*state));

	if (!view || !state) {
		morph_tui_core_builder_fail(builder, -ENOMEM);
		return NULL;
	}
	view->ops = ops;
	view->state = state;
	state->controller = builder->controller;
	state->parent = parent;
	state->options = opts;
	int rc = morph_array_init(&state->children, 4, sizeof(view));
	struct morph_tui_core_view **slot = rc ? NULL : morph_array_push(&scope->views);

	if (!slot) {
		morph_array_cleanup(&state->children);
		morph_tui_core_builder_fail(builder, rc ? rc : -ENOMEM);
		return NULL;
	}
	*slot = view;
	if (parent) {
		slot = morph_array_push(&parent->state->children);
		if (slot)
			*slot = view;
		else
			morph_tui_core_builder_fail(builder, -ENOMEM);
	} else {
		scope->root = view;
	}
	return view;
}

struct morph_tui_core_view_controller *
morph_tui_core_view_owner(const struct morph_tui_core_view *view)
{
	return view && view->state ? view->state->controller : NULL;
}

size_t morph_tui_core_view_child_count(const struct morph_tui_core_view *view)
{
	return view && view->state ? view->state->children.nelts : 0;
}

struct morph_tui_core_view *morph_tui_core_view_child(const struct morph_tui_core_view *view,
						      size_t index)
{
	struct morph_tui_core_view **slot =
		view ? morph_array_get(&view->state->children, index) : NULL;

	return slot ? *slot : NULL;
}

struct morph_tui_core_view_rect morph_tui_core_view_frame(const struct morph_tui_core_view *view)
{
	return view ? view->state->frame : (struct morph_tui_core_view_rect){0};
}

struct morph_tui_core_view_options
morph_tui_core_view_get_options(const struct morph_tui_core_view *view)
{
	return view ? view->state->options : (struct morph_tui_core_view_options){0};
}

int morph_tui_core_view_is_owner(const struct morph_tui_core_view *view)
{
	return view && view->state &&
	       morph_tui_core_app_is_owner(view->state->controller->state->app);
}

static int core_view_extent(int value, int minimum, int maximum)
{
	if (value < minimum)
		value = minimum;
	if (maximum && value > maximum)
		value = maximum;
	return value;
}

int morph_tui_core_view_measure(struct morph_tui_core_view *view, int width,
				struct morph_tui_core_view_size *size)
{
	if (!view || !size || width < 0)
		return -EINVAL;
	if (!morph_tui_core_view_is_owner(view))
		return -EPERM;
	*size = (struct morph_tui_core_view_size){0};
	if (view->state->hidden)
		return 0;
	struct morph_tui_core_view_options opts = view->state->options;
	int inner = width > opts.padding * 2 ? width - opts.padding * 2 : 0;
	struct morph_tui_core_layout *manager = view->state->layout;
	int rc = 0;

	if (manager && manager->ops && manager->ops->measure)
		rc = manager->ops->measure(manager, view, inner, size);
	else if (view->ops && view->ops->measure)
		rc = view->ops->measure(view, inner, size);
	if (rc)
		return rc;
	if (size->width < 0 || size->height < 0 || size->width > INT_MAX - 2 * opts.padding ||
	    size->height > INT_MAX - 2 * opts.padding)
		return -EOVERFLOW;
	size->width += opts.padding * 2;
	size->height += opts.padding * 2;
	if (opts.width)
		size->width = opts.width;
	if (opts.height)
		size->height = opts.height;
	size->width = core_view_extent(size->width, opts.min_width, opts.max_width);
	size->height = core_view_extent(size->height, opts.min_height, opts.max_height);
	return 0;
}

int morph_tui_core_view_arrange(struct morph_tui_core_view *view,
				struct morph_tui_core_view_rect frame)
{
	if (!view || frame.x < 0 || frame.y < 0 || frame.width < 0 || frame.height < 0)
		return -EINVAL;
	if (frame.x > INT_MAX - frame.width || frame.y > INT_MAX - frame.height)
		return -EOVERFLOW;
	if (!morph_tui_core_view_is_owner(view))
		return -EPERM;
	view->state->frame = frame;
	struct morph_tui_core_layout *manager = view->state->layout;

	if (manager && manager->ops && manager->ops->arrange)
		return manager->ops->arrange(manager, view);
	if (view->ops && view->ops->layout)
		return view->ops->layout(view);
	return 0;
}

void morph_tui_core_view_invalidate(struct morph_tui_core_view *view, int layout)
{
	if (!view)
		return;
	morph_tui_core_app_invalidate(view->state->controller->state->app,
				      MORPH_TUI_CORE_INVALIDATE_DISPLAY |
					      (layout ? MORPH_TUI_CORE_INVALIDATE_LAYOUT : 0));
}

int morph_tui_core_view_hidden(const struct morph_tui_core_view *view)
{
	return !view || view->state->hidden;
}

int morph_tui_core_view_set_hidden(struct morph_tui_core_view *view, int hidden)
{
	if (!view)
		return -EINVAL;
	if (!morph_tui_core_view_is_owner(view))
		return -EPERM;
	view->state->hidden = !!hidden;
	if (!core_view_can_focus(view->state->controller->state->focus))
		view->state->controller->state->focus = NULL;
	morph_tui_core_view_invalidate(view, 1);
	return 0;
}

int core_view_can_focus(const struct morph_tui_core_view *view)
{
	if (!view || !view->state->focusable)
		return 0;
	for (const struct morph_tui_core_view *ancestor = view; ancestor;
	     ancestor = ancestor->state->parent)
		if (ancestor->state->hidden)
			return 0;
	return 1;
}

int morph_tui_core_view_set_focusable(struct morph_tui_core_view *view, int focusable)
{
	if (!view)
		return -EINVAL;
	if (!morph_tui_core_view_is_owner(view))
		return -EPERM;
	view->state->focusable = !!focusable;
	if (!focusable && morph_tui_core_view_focused(view))
		view->state->controller->state->focus = NULL;
	morph_tui_core_view_invalidate(view, 0);
	return 0;
}

int morph_tui_core_view_focus(struct morph_tui_core_view *view)
{
	if (!core_view_can_focus(view))
		return -EINVAL;
	if (!morph_tui_core_view_is_owner(view))
		return -EPERM;
	view->state->controller->state->focus = view;
	morph_tui_core_view_invalidate(view, 0);
	return 0;
}

int morph_tui_core_view_focused(const struct morph_tui_core_view *view)
{
	return view && view->state->controller->state->focus == view;
}

int core_view_focus_next(struct morph_tui_core_view_controller *controller, int backwards)
{
	struct morph_tui_core_view_controller_state *scope = controller->state;
	struct morph_tui_core_view *view = scope->root, *previous = NULL, *first = NULL;
	struct morph_tui_core_view *after = NULL;
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
					return morph_tui_core_view_focus(previous);
				found = 1;
			}
			previous = view;
		}
		if (morph_tui_core_view_child_count(view)) {
			view = morph_tui_core_view_child(view, 0);
			continue;
		}
		struct morph_tui_core_view *next = NULL;

		while (view && view->state->parent && !next) {
			struct morph_tui_core_view *parent = view->state->parent;

			for (size_t i = 0; i + 1 < morph_tui_core_view_child_count(parent); i++)
				if (morph_tui_core_view_child(parent, i) == view) {
					next = morph_tui_core_view_child(parent, i + 1);
					break;
				}
			view = parent;
		}
		view = next;
	}
	struct morph_tui_core_view *target = backwards ? previous : (after ? after : first);

	if (target)
		return morph_tui_core_view_focus(target);
	return 0;
}

int morph_tui_core_view_set_layout(struct morph_tui_core_view *view,
				   struct morph_tui_core_layout *layout)
{
	if (!view || (layout && !layout->ops))
		return -EINVAL;
	if (!morph_tui_core_view_is_owner(view))
		return -EPERM;
	view->state->layout = layout;
	morph_tui_core_view_invalidate(view, 1);
	return 0;
}

int core_view_draw(struct morph_tui_core_view *view, struct morph_tui_render_canvas *parent)
{
	if (view->state->hidden)
		return 0;
	struct morph_tui_core_view_rect frame = view->state->frame;
	struct morph_tui_render_canvas canvas;

	morph_tui_render_canvas_child(parent, frame.x, frame.y, frame.width, frame.height, &canvas);
	if (canvas.left >= canvas.right || canvas.top >= canvas.bottom)
		return 0;
	int pad = view->state->options.padding;
	struct morph_tui_render_canvas content;

	morph_tui_render_canvas_child(
		&canvas, pad, pad, frame.width > pad * 2 ? frame.width - pad * 2 : 0,
		frame.height > pad * 2 ? frame.height - pad * 2 : 0, &content);
	int rc = view->ops && view->ops->draw ? view->ops->draw(view, &content) : 0;

	for (size_t i = 0; i < view->state->children.nelts && !rc; i++)
		rc = core_view_draw(morph_tui_core_view_child(view, i), &canvas);
	return rc;
}

void core_view_scope_destroy(struct morph_tui_core_view_controller *controller)
{
	for (size_t i = controller->state->views.nelts; i > 0; i--) {
		struct morph_tui_core_view **slot =
			morph_array_get(&controller->state->views, i - 1);

		if ((*slot)->ops && (*slot)->ops->destroy)
			(*slot)->ops->destroy(*slot);
		morph_array_cleanup(&(*slot)->state->children);
	}
	morph_array_cleanup(&controller->state->views);
	arena_destroy(controller->state->arena);
}
