#include <morph/tui/widget.h>
#include "tui/core/view_internal.h"
#include "util/error.h"

#include <stdlib.h>
#include <string.h>

struct morph_tui_widget *morph_tui_widget_create(struct morph_tui_builder *builder,
						 struct morph_tui_view *parent, size_t size,
						 const struct morph_tui_view_ops *ops,
						 const struct morph_tui_view_options *options)
{
	return (struct morph_tui_widget *)morph_tui_view_create(builder, parent, size, ops,
								options);
}

int morph_tui_widget_set_tag(struct morph_tui_view *view, int tag)
{
	if (!view)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	view->state->tag = tag;
	return 0;
}

int morph_tui_widget_emit(struct morph_tui_view *view, enum morph_tui_widget_action_type type,
			  size_t index, const char *text)
{
	if (!view)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct morph_tui_view_controller *controller = view->state->controller;
	if (!view->state->action && !controller->ops->action)
		return 0;
	char *copy = text ? strdup(text) : NULL;

	if (text && !copy)
		return -ENOMEM;
	struct morph_tui_widget_action action = {
		.sender = view,
		.type = type,
		.tag = view->state->tag,
		.index = index,
		.text = copy,
	};
	int rc = view->state->action ? view->state->action(view->state->action_data, &action)
				     : controller->ops->action(controller, &action);

	free(copy);
	return rc;
}

int morph_tui_widget_set_action(struct morph_tui_view *view,
				int (*action)(void *, const struct morph_tui_widget_action *),
				void *data)
{
	if (!view)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	view->state->action = action;
	view->state->action_data = data;
	return 0;
}
