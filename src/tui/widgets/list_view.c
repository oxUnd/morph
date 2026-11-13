#include <morph/tui/widgets/list.h>
#include "util/error.h"
#include "util/utf8.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct widget_list_view {
	struct morph_tui_widget base;
	struct morph_tui_widget_menu_state menu;
	char **items;
	size_t owned_count;
	morph_tui_widget_list_source_fn source;
	void *source_data;
	int mouse_pressed;
	size_t pressed_index;
};

static const char *widget_list_view_text(struct widget_list_view *list, size_t index)
{
	const char *text =
		list->source ? list->source(list->source_data, index) : list->items[index];

	return text ? text : "";
}

static int widget_list_view_measure(struct morph_tui_view *view, int width,
				    struct morph_tui_view_size *size)
{
	struct widget_list_view *list = (struct widget_list_view *)view;

	/* Lists virtualize their rows; measurement never enumerates a data source. */
	*size = (struct morph_tui_view_size){
		width, list->menu.list.count > INT_MAX ? INT_MAX : (int)list->menu.list.count};
	return 0;
}

static int widget_list_view_layout(struct morph_tui_view *view)
{
	struct widget_list_view *list = (struct widget_list_view *)view;
	struct morph_tui_view_rect content = morph_tui_view_content_rect(view);

	morph_tui_widget_list_state_layout(&list->menu.list, list->menu.list.count,
					   content.height > 0 ? (size_t)content.height : 1);
	return 0;
}

static int widget_list_view_draw(struct morph_tui_view *view, struct morph_tui_canvas *canvas)
{
	struct widget_list_view *list = (struct widget_list_view *)view;

	for (int row = 0; row < canvas->height; row++) {
		size_t index = list->menu.list.first + (size_t)row;

		if (index >= list->menu.list.count)
			break;
		int selected = morph_tui_view_focused(view) && index == list->menu.list.index;
		struct morph_tui_text_style style = {
			.attributes = selected ? MORPH_TUI_STYLE_BOLD : MORPH_TUI_STYLE_NORMAL,
			.foreground = {selected ? MORPH_TUI_COLOR_INDEXED : MORPH_TUI_COLOR_INHERIT,
				       selected ? 6U : 0U},
		};
		struct morph_tui_canvas line;

		morph_tui_canvas_child_borrow(canvas, 0, row, canvas->width, 1, &line);
		int rc = morph_tui_canvas_fill_styled(&line, &style);

		if (!rc)
			rc = morph_tui_canvas_text_styled(&line, 0, 0,
						  widget_list_view_text(list, index), &style);
		if (rc)
			return rc;
	}
	return 0;
}

static int widget_list_view_mouse(struct morph_tui_view *view,
				  const struct morph_tui_mouse_event *mouse)
{
	struct widget_list_view *list = (struct widget_list_view *)view;
	struct morph_tui_view_rect content = morph_tui_view_content_rect(view);
	int inside = mouse->x >= 0 && mouse->x < content.width && mouse->y >= 0 &&
		     mouse->y < content.height;
	size_t index = inside ? list->menu.list.first + (size_t)mouse->y : SIZE_MAX;

	if (mouse->action == MORPH_TUI_MOUSE_CANCEL) {
		list->mouse_pressed = 0;
		return 0;
	}
	if (mouse->button != MORPH_TUI_MOUSE_LEFT || mouse->modifiers) {
		list->mouse_pressed = 0;
		return 0;
	}
	inside = inside && index < list->menu.list.count;
	if (mouse->action == MORPH_TUI_MOUSE_PRESS) {
		list->mouse_pressed = inside;
		if (!inside)
			return 0;
		list->pressed_index = index;
		int changed = list->menu.list.index != index;

		list->menu.list.index = index;
		morph_tui_view_invalidate(view, 0);
		int rc = changed ? morph_tui_widget_emit(view, MORPH_TUI_WIDGET_ACTION_CHANGED,
							index, widget_list_view_text(list, index))
				 : 0;

		return rc ? rc : 1;
	}
	if (mouse->action == MORPH_TUI_MOUSE_MOVE) {
		list->mouse_pressed = 0;
		return 0;
	}
	int activate = mouse->action == MORPH_TUI_MOUSE_RELEASE && list->mouse_pressed &&
		       inside && index == list->pressed_index;

	list->mouse_pressed = 0;
	if (!activate)
		return 0;
	int rc = morph_tui_widget_emit(view, MORPH_TUI_WIDGET_ACTION_ACTIVATE, index,
				       widget_list_view_text(list, index));

	return rc ? rc : 1;
}

static int widget_list_view_event(struct morph_tui_view *view, const struct morph_tui_event *event)
{
	struct widget_list_view *list = (struct widget_list_view *)view;

	if (event->type == MORPH_TUI_EVENT_MOUSE)
		return widget_list_view_mouse(view, &event->mouse);
	if (event->type != MORPH_TUI_EVENT_KEY && event->type != MORPH_TUI_EVENT_TEXT)
		return 0;
	if (event->pasting)
		return 0;
	list->mouse_pressed = 0;
	int key = event->key == MORPH_TUI_KEY_WHEEL_UP ? MORPH_TUI_KEY_UP
		  : event->key == MORPH_TUI_KEY_WHEEL_DOWN ? MORPH_TUI_KEY_DOWN
							  : event->key;
	int result = morph_tui_widget_menu_state_update(&list->menu, key);

	if (result != MORPH_TUI_WIDGET_CHANGED && result != MORPH_TUI_WIDGET_SUBMIT)
		return 0;
	morph_tui_view_invalidate(view, 0);
	int rc = morph_tui_widget_emit(
		view,
		result == MORPH_TUI_WIDGET_SUBMIT ? MORPH_TUI_WIDGET_ACTION_ACTIVATE
						  : MORPH_TUI_WIDGET_ACTION_CHANGED,
		list->menu.list.index, widget_list_view_text(list, list->menu.list.index));

	return rc ? rc : 1;
}

static void widget_list_view_clear(struct widget_list_view *list)
{
	for (size_t i = 0; i < list->owned_count; i++)
		free(list->items[i]);
	free(list->items);
	list->items = NULL;
	list->owned_count = 0;
	list->mouse_pressed = 0;
}

static void widget_list_view_destroy(struct morph_tui_view *view)
{
	widget_list_view_clear((struct widget_list_view *)view);
}

static int widget_list_view_valid(const struct morph_tui_view *view)
{
	return view && view->ops && view->ops->draw == widget_list_view_draw;
}

int morph_tui_widget_list_set_items_copy(struct morph_tui_view *view, const char *const *items,
					 size_t count)
{
	if (!widget_list_view_valid(view) || (count && !items))
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct widget_list_view replacement = {0};

	if (count) {
		replacement.items = calloc(count, sizeof(*replacement.items));
		if (!replacement.items)
			return -ENOMEM;
	}
	int rc = 0;

	for (size_t i = 0; i < count && !rc; i++) {
		if (!items[i])
			rc = -EINVAL;
		else {
			replacement.items[i] = utf8_terminal_sanitize_dup(
				items[i], strlen(items[i]), UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
			if (!replacement.items[i])
				rc = -ENOMEM;
			else
				replacement.owned_count++;
		}
	}
	if (rc) {
		widget_list_view_clear(&replacement);
		return rc;
	}
	struct widget_list_view *list = (struct widget_list_view *)view;
	int equal = !list->source && list->owned_count == count;
	for (size_t i = 0; equal && i < count; i++)
		equal = !strcmp(list->items[i], replacement.items[i]);
	if (equal) {
		widget_list_view_clear(&replacement);
		return 0;
	}

	widget_list_view_clear(list);
	list->items = replacement.items;
	list->owned_count = count;
	list->source = NULL;
	list->source_data = NULL;
	morph_tui_widget_list_state_layout(&list->menu.list, count, list->menu.list.page);
	morph_tui_view_invalidate(view, 1);
	return 0;
}

int morph_tui_widget_list_set_source_borrow(struct morph_tui_view *view, size_t count,
					    morph_tui_widget_list_source_fn text, void *data)
{
	if (!widget_list_view_valid(view) || (count && !text))
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct widget_list_view *list = (struct widget_list_view *)view;
	int resized = list->menu.list.count != count;

	widget_list_view_clear(list);
	list->source = text;
	list->source_data = data;
	morph_tui_widget_list_state_layout(&list->menu.list, count, list->menu.list.page);
	morph_tui_view_invalidate(view, resized);
	return 0;
}

size_t morph_tui_widget_list_selected(const struct morph_tui_view *view)
{
	return widget_list_view_valid(view)
		       ? ((const struct widget_list_view *)view)->menu.list.index
		       : 0;
}

int morph_tui_widget_list_set_navigation(struct morph_tui_view *view, unsigned flags)
{
	if (!widget_list_view_valid(view))
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	((struct widget_list_view *)view)->menu.list.flags = flags;
	return 0;
}

int morph_tui_widget_list_select(struct morph_tui_view *view, size_t index)
{
	if (!widget_list_view_valid(view))
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct widget_list_view *list = (struct widget_list_view *)view;

	if (index >= list->menu.list.count)
		return -ERANGE;
	if (list->menu.list.index == index)
		return 0;
	list->mouse_pressed = 0;
	list->menu.list.index = index;
	morph_tui_widget_list_state_layout(&list->menu.list, list->menu.list.count,
					   list->menu.list.page);
	morph_tui_view_invalidate(view, 0);
	return 0;
}

struct morph_tui_view *
morph_tui_widget_list_add_borrow(struct morph_tui_builder *builder, struct morph_tui_view *parent,
				 const char *const *items, size_t count, int tag,
				 const struct morph_tui_view_options *options)
{
	static const struct morph_tui_view_ops ops = {
		.measure = widget_list_view_measure,
		.layout = widget_list_view_layout,
		.draw = widget_list_view_draw,
		.event = widget_list_view_event,
		.destroy = widget_list_view_destroy,
	};
	struct widget_list_view *list = NULL;

	list = (struct widget_list_view *)morph_tui_widget_add_borrow(builder, parent,
								      sizeof(*list), &ops, options);
	if (!list)
		return NULL;
	struct morph_tui_view *view = &list->base.view;

	list->menu.list.flags = MORPH_TUI_WIDGET_LIST_VIM;
	morph_tui_view_set_focusable(view, 1);
	morph_tui_widget_set_tag(view, tag);
	morph_tui_builder_fail(builder, morph_tui_widget_list_set_items_copy(view, items, count));
	return view;
}
