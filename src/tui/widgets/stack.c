#include <morph/tui/widgets/stack.h>
#include <morph/tui/layout.h>
#include "util/array.h"
#include "util/error.h"

#include <limits.h>
#include <stdint.h>

struct widget_stack_item {
	struct morph_tui_view *view;
	struct morph_tui_view_size measured;
	int span, weight, maximum;
};

struct widget_stack {
	struct morph_tui_widget base;
	struct morph_tui_layout layout;
	morph_array_t plan;
	enum morph_tui_widget_stack_axis axis;
	int spacing;
};

static int widget_stack_measure(struct morph_tui_layout *layout, struct morph_tui_view *view,
				int width, struct morph_tui_view_size *size)
{
	struct widget_stack *stack = (struct widget_stack *)view;
	int64_t main = 0;
	int cross = 0, count = 0;

	(void)layout;
	for (size_t i = 0; i < morph_tui_view_child_count(view); i++) {
		struct morph_tui_view *child = morph_tui_view_child_borrow(view, i);
		struct morph_tui_view_size measured;

		if (morph_tui_view_hidden(child))
			continue;
		int rc = morph_tui_view_measure(child, width, &measured);

		if (rc)
			return rc;
		int child_main = stack->axis == MORPH_TUI_WIDGET_STACK_VERTICAL ? measured.height
										: measured.width;
		int child_cross = stack->axis == MORPH_TUI_WIDGET_STACK_VERTICAL ? measured.width
										 : measured.height;

		main += child_main;
		if (count++)
			main += stack->spacing;
		if (cross < child_cross)
			cross = child_cross;
	}
	if (main > INT_MAX)
		return -EOVERFLOW;
	*size = stack->axis == MORPH_TUI_WIDGET_STACK_VERTICAL
			? (struct morph_tui_view_size){cross, (int)main}
			: (struct morph_tui_view_size){(int)main, cross};
	return 0;
}

static int widget_stack_arrange(struct morph_tui_layout *layout, struct morph_tui_view *view)
{
	struct widget_stack *stack = (struct widget_stack *)view;
	struct morph_tui_view_rect content = morph_tui_view_content_rect(view);
	int vertical = stack->axis == MORPH_TUI_WIDGET_STACK_VERTICAL;
	int width = content.width;
	int height = content.height;
	int available = vertical ? height : width;
	int64_t used = 0;

	(void)layout;
	morph_array_clear(&stack->plan);
	for (size_t i = 0; i < morph_tui_view_child_count(view); i++) {
		struct morph_tui_view *child = morph_tui_view_child_borrow(view, i);
		struct morph_tui_view_size measured;

		if (morph_tui_view_hidden(child))
			continue;
		int rc = morph_tui_view_measure(child, width, &measured);

		if (rc)
			return rc;
		struct morph_tui_view_options opts = morph_tui_view_get_options(child);
		int fixed = vertical ? opts.height : opts.width;
		int weight = fixed ? 0 : opts.grow;
		int margin = vertical ? opts.margin.top + opts.margin.bottom
				      : opts.margin.left + opts.margin.right;
		int maximum = vertical ? opts.max_height : opts.max_width;
		int span = weight ? (vertical ? opts.min_height : opts.min_width) + margin
				  : (vertical ? measured.height : measured.width);
		struct widget_stack_item *item = morph_array_push_borrow(&stack->plan);

		if (!item)
			return -ENOMEM;
		*item = (struct widget_stack_item){
			.view = child, .measured = measured, .span = span, .weight = weight,
			.maximum = maximum ? maximum + margin : 0};
		used += span;
		if (stack->plan.nelts > 1)
			used += stack->spacing;
	}
	int remaining = used < available ? available - (int)used : 0;

	while (remaining > 0) {
		int64_t weight = 0;

		for (size_t i = 0; i < stack->plan.nelts; i++) {
			struct widget_stack_item *item = morph_array_borrow(&stack->plan, i);

			if (!item->maximum || item->span < item->maximum)
				weight += item->weight;
		}
		if (!weight)
			break;
		int budget = remaining;

		for (size_t i = 0; i < stack->plan.nelts && remaining; i++) {
			struct widget_stack_item *item = morph_array_borrow(&stack->plan, i);

			if (!item->weight || (item->maximum && item->span >= item->maximum))
				continue;
			int share = (int)((int64_t)budget * item->weight / weight);

			if (!share)
				share = 1;
			if (item->maximum && share > item->maximum - item->span)
				share = item->maximum - item->span;
			if (share > remaining)
				share = remaining;
			item->span += share;
			remaining -= share;
		}
	}
	int offset = 0;

	for (size_t i = 0; i < stack->plan.nelts; i++) {
		struct widget_stack_item item =
			*(struct widget_stack_item *)morph_array_borrow(&stack->plan, i);
		struct morph_tui_view_options opts = morph_tui_view_get_options(item.view);
		int cross = vertical ? width : height;
		int fixed = vertical ? opts.width : opts.height;
		int maximum = vertical ? opts.max_width : opts.max_height;
		int margin = vertical ? opts.margin.left + opts.margin.right
				      : opts.margin.top + opts.margin.bottom;
		int full_cross = cross;
		int intrinsic = vertical ? item.measured.width : item.measured.height;

		if (opts.alignment != MORPH_TUI_VIEW_STRETCH && cross > intrinsic)
			cross = intrinsic;

		if (fixed && cross > fixed + margin)
			cross = fixed + margin;
		if (maximum && cross > maximum + margin)
			cross = maximum + margin;
		int span = item.span < available - offset ? item.span : available - offset;
		int cross_offset = opts.alignment == MORPH_TUI_VIEW_CENTER
					   ? (full_cross - cross) / 2
				   : opts.alignment == MORPH_TUI_VIEW_END ? full_cross - cross
									  : 0;
		struct morph_tui_view_rect rect =
			vertical ? (struct morph_tui_view_rect){content.x + cross_offset,
								content.y + offset, cross, span}
				 : (struct morph_tui_view_rect){content.x + offset,
								content.y + cross_offset, span,
								cross};
		int rc = morph_tui_view_arrange(item.view, rect);

		if (rc)
			return rc;
		offset += span;
		int gap = stack->spacing < available - offset ? stack->spacing : available - offset;

		offset += gap;
	}
	return 0;
}

static void widget_stack_destroy(struct morph_tui_view *view)
{
	morph_array_cleanup(&((struct widget_stack *)view)->plan);
}

struct morph_tui_view *
morph_tui_widget_stack_add_borrow(struct morph_tui_builder *builder, struct morph_tui_view *parent,
				  enum morph_tui_widget_stack_axis axis, int spacing,
				  const struct morph_tui_view_options *options)
{
	static const struct morph_tui_view_ops ops = {.destroy = widget_stack_destroy};
	static const struct morph_tui_layout_ops layout = {
		.measure = widget_stack_measure,
		.arrange = widget_stack_arrange,
	};
	struct widget_stack *stack = NULL;
	struct widget_stack_item item;

	if (spacing < 0 || (axis != MORPH_TUI_WIDGET_STACK_VERTICAL &&
			    axis != MORPH_TUI_WIDGET_STACK_HORIZONTAL)) {
		morph_tui_builder_fail(builder, -EINVAL);
		return NULL;
	}
	stack = (struct widget_stack *)morph_tui_widget_add_borrow(builder, parent, sizeof(*stack),
								   &ops, options);
	if (!stack)
		return NULL;
	stack->axis = axis;
	stack->spacing = spacing;
	stack->layout.ops = &layout;
	morph_tui_builder_fail(builder, morph_array_init(&stack->plan, 8, sizeof(item)));
	morph_tui_view_set_layout_borrow(&stack->base.view, &stack->layout);
	return &stack->base.view;
}
