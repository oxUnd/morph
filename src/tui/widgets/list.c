#include <morph/tui/widgets/list.h>

void morph_tui_widget_list_state_layout(struct morph_tui_widget_list_state *list, size_t count,
					size_t page)
{
	list->count = count;
	list->page = page ? page : 1;
	if (!count) {
		list->index = 0;
		list->first = 0;
		return;
	}
	if (list->index >= count)
		list->index = count - 1;
	if (list->flags & MORPH_TUI_WIDGET_LIST_PAGED) {
		list->first = list->index / list->page * list->page;
	} else {
		if (list->first > list->index)
			list->first = list->index;
		if (list->index - list->first >= list->page)
			list->first = list->index - list->page + 1;
		if (list->first > (count > list->page ? count - list->page : 0))
			list->first = count > list->page ? count - list->page : 0;
	}
}

int morph_tui_widget_list_state_update(struct morph_tui_widget_list_state *list, int key)
{
	size_t old = list->index;
	size_t step = 1;
	int up = key == MORPH_TUI_KEY_UP;
	int down = key == MORPH_TUI_KEY_DOWN;

	if (!list->count)
		return 0;
	if (list->flags & MORPH_TUI_WIDGET_LIST_VIM) {
		up |= key == 'k';
		down |= key == 'j';
	}
	if (list->flags & MORPH_TUI_WIDGET_LIST_TAB)
		down |= key == '\t';
	if (key == MORPH_TUI_KEY_PAGE_UP || key == MORPH_TUI_KEY_PAGE_DOWN ||
	    ((list->flags & MORPH_TUI_WIDGET_LIST_HORIZONTAL_PAGES) &&
	     (key == MORPH_TUI_KEY_LEFT || key == MORPH_TUI_KEY_RIGHT))) {
		step = list->page ? list->page : 1;
		up = key == MORPH_TUI_KEY_PAGE_UP || key == MORPH_TUI_KEY_LEFT;
		down = !up;
	}
	if (up) {
		list->index = (list->flags & MORPH_TUI_WIDGET_LIST_WRAP) && !list->index
				      ? list->count - 1
			      : list->index > step ? list->index - step
						   : 0;
	}
	if (down) {
		list->index =
			(list->flags & MORPH_TUI_WIDGET_LIST_WRAP) && list->index == list->count - 1
				? 0
			: step < list->count - list->index ? list->index + step
							   : list->count - 1;
	}
	if (key == MORPH_TUI_KEY_HOME)
		list->index = 0;
	if (key == MORPH_TUI_KEY_END)
		list->index = list->count - 1;
	if ((list->flags & MORPH_TUI_WIDGET_LIST_DIGITS) && key >= '1' && key <= '9' &&
	    (size_t)(key - '1') < list->count)
		list->index = (size_t)(key - '1');
	morph_tui_widget_list_state_layout(list, list->count, list->page);
	return old != list->index;
}

size_t morph_tui_widget_scroll_update(size_t offset, size_t count, size_t page, int key)
{
	struct morph_tui_widget_list_state scroll = {0};

	if (!page)
		page = 1;

	scroll.index = offset;
	scroll.flags = MORPH_TUI_WIDGET_LIST_VIM;
	morph_tui_widget_list_state_layout(&scroll, count > page ? count - page + 1 : 1, page);
	morph_tui_widget_list_state_update(&scroll, key);
	return scroll.index;
}

int morph_tui_widget_menu_state_update(struct morph_tui_widget_menu_state *menu, int key)
{
	if (key == MORPH_TUI_KEY_PASTE_BEGIN) {
		menu->pasting = 1;
		return MORPH_TUI_WIDGET_UNHANDLED;
	}
	if (key == MORPH_TUI_KEY_PASTE_END) {
		menu->pasting = 0;
		return MORPH_TUI_WIDGET_UNHANDLED;
	}
	if (menu->pasting)
		return MORPH_TUI_WIDGET_UNHANDLED;
	if (key == 3 || key == 4)
		return MORPH_TUI_WIDGET_CANCEL;
	if (key == 27)
		return MORPH_TUI_WIDGET_BACK;
	if ((key == '\r' || key == '\n') && menu->list.count)
		return MORPH_TUI_WIDGET_SUBMIT;
	return morph_tui_widget_list_state_update(&menu->list, key) ? MORPH_TUI_WIDGET_CHANGED
								    : MORPH_TUI_WIDGET_UNHANDLED;
}
