#include <morph/tui/widgets/list.h>
#include "nb/err.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

static int morph_tui_widget_list_key_forward(const struct morph_tui_widget_list_state *list,
					      int key)
{
	return !(key == MORPH_TUI_KEY_UP || key == MORPH_TUI_KEY_PAGE_UP ||
		 key == MORPH_TUI_KEY_LEFT ||
		 ((list->flags & MORPH_TUI_WIDGET_LIST_VIM) && key == 'k'));
}

static size_t morph_tui_widget_list_groups_snap(const struct morph_tui_widget_list_groups *groups,
						size_t index, int forward)
{
	if (!groups->row_count)
		return 0;
	if (index >= groups->row_count)
		index = groups->row_count - 1;
	if (groups->row_item[index] != SIZE_MAX)
		return index;
	if (forward) {
		for (size_t i = index + 1; i < groups->row_count; i++) {
			if (groups->row_item[i] != SIZE_MAX)
				return i;
		}
		for (size_t i = index; i > 0; i--) {
			if (groups->row_item[i - 1] != SIZE_MAX)
				return i - 1;
		}
	} else {
		for (size_t i = index; i > 0; i--) {
			if (groups->row_item[i - 1] != SIZE_MAX)
				return i - 1;
		}
		for (size_t i = index + 1; i < groups->row_count; i++) {
			if (groups->row_item[i] != SIZE_MAX)
				return i;
		}
	}
	return index;
}

int morph_tui_widget_list_groups_init(struct morph_tui_widget_list_groups *groups,
				      const size_t *counts, size_t group_count)
{
	size_t row_count = group_count;
	size_t item_count = 0;
	size_t row = 0;
	size_t item = 0;

	if (!groups || (group_count && !counts))
		return NB_ERR_INVAL;
	memset(groups, 0, sizeof(*groups));
	for (size_t g = 0; g < group_count; g++) {
		if (counts[g] > SIZE_MAX - row_count)
			return NB_ERR_RANGE;
		row_count += counts[g];
		if (counts[g] > SIZE_MAX - item_count)
			return NB_ERR_RANGE;
		item_count += counts[g];
	}
	if (row_count > SIZE_MAX / sizeof(*groups->row_item) ||
	    item_count > SIZE_MAX / sizeof(*groups->item_row))
		return NB_ERR_RANGE;
	groups->row_item = calloc(row_count ? row_count : 1, sizeof(*groups->row_item));
	groups->row_group = calloc(row_count ? row_count : 1, sizeof(*groups->row_group));
	groups->item_row =
		item_count ? calloc(item_count, sizeof(*groups->item_row)) : NULL;
	if (!groups->row_item || !groups->row_group ||
	    (item_count && !groups->item_row)) {
		morph_tui_widget_list_groups_cleanup(groups);
		return NB_ERR_NOMEM;
	}
	for (size_t g = 0; g < group_count; g++) {
		groups->row_item[row] = SIZE_MAX;
		groups->row_group[row] = g;
		row++;
		for (size_t i = 0; i < counts[g]; i++) {
			groups->row_item[row] = item;
			groups->row_group[row] = g;
			groups->item_row[item] = row;
			row++;
			item++;
		}
	}
	groups->group_count = group_count;
	groups->row_count = row_count;
	groups->item_count = item_count;
	return 0;
}

void morph_tui_widget_list_groups_cleanup(struct morph_tui_widget_list_groups *groups)
{
	if (!groups)
		return;
	free(groups->row_item);
	free(groups->row_group);
	free(groups->item_row);
	memset(groups, 0, sizeof(*groups));
}

void morph_tui_widget_list_state_layout_groups(struct morph_tui_widget_list_state *list,
					       const struct morph_tui_widget_list_groups *groups,
					       size_t page)
{
	if (!list || !groups)
		return;
	list->count = groups->row_count;
	list->page = page ? page : 1;
	morph_tui_widget_list_state_layout(list, groups->row_count, list->page);
	if (groups->item_count && groups->row_item[list->index] == SIZE_MAX)
		list->index = morph_tui_widget_list_groups_snap(groups, list->index, 1);
	morph_tui_widget_list_state_layout(list, groups->row_count, list->page);
}

int morph_tui_widget_list_state_update_groups(struct morph_tui_widget_list_state *list,
					      const struct morph_tui_widget_list_groups *groups,
					      size_t page, int key)
{
	size_t old;

	if (!list || !groups)
		return 0;
	old = list->index;
	list->count = groups->row_count;
	list->page = page ? page : 1;
	if (!groups->item_count)
		return 0;
	(void)morph_tui_widget_list_state_update(list, key);
	if (groups->item_count && groups->row_item[list->index] == SIZE_MAX) {
		size_t index = morph_tui_widget_list_groups_snap(
			groups, list->index, morph_tui_widget_list_key_forward(list, key));

		if (index != list->index) {
			list->index = index;
		}
	}
	morph_tui_widget_list_state_layout(list, groups->row_count, list->page);
	return old != list->index;
}

int morph_tui_widget_menu_state_update_groups(struct morph_tui_widget_menu_state *menu,
					      const struct morph_tui_widget_list_groups *groups,
					      size_t page, int key)
{
	int result;
	size_t old;

	if (!menu || !groups)
		return MORPH_TUI_WIDGET_UNHANDLED;
	old = menu->list.index;
	menu->list.count = groups->row_count;
	menu->list.page = page ? page : 1;
	result = morph_tui_widget_menu_state_update(menu, key);
	if (!groups->item_count &&
	    (result == MORPH_TUI_WIDGET_CHANGED || result == MORPH_TUI_WIDGET_SUBMIT))
		return MORPH_TUI_WIDGET_UNHANDLED;
	if ((result == MORPH_TUI_WIDGET_CHANGED || result == MORPH_TUI_WIDGET_SUBMIT) &&
	    groups->item_count && groups->row_item[menu->list.index] == SIZE_MAX) {
		menu->list.index = morph_tui_widget_list_groups_snap(
			groups, menu->list.index,
			morph_tui_widget_list_key_forward(&menu->list, key));
		morph_tui_widget_list_state_layout(&menu->list, groups->row_count,
						   menu->list.page);
	}
	if (result == MORPH_TUI_WIDGET_CHANGED && old == menu->list.index)
		return MORPH_TUI_WIDGET_UNHANDLED;
	return result;
}
