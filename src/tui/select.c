#include <morph/tui/components.h>
#include <string.h>

int morph_tui_select_init(struct morph_tui_select *select, size_t count,
			  int multi, size_t minimum, size_t maximum)
{
	if (!select || !count || minimum > count || maximum > count ||
	    (maximum && maximum < minimum) || (!multi && (minimum > 1 || maximum > 1)))
		return -EINVAL;
	*select = (struct morph_tui_select){0};
	select->multi = multi != 0;
	select->selected_count = multi ? 0 : 1;
	select->minimum = minimum;
	select->maximum = multi ? (maximum ? maximum : count) : 1;
	select->menu.list.flags = MORPH_TUI_LIST_WRAP | MORPH_TUI_LIST_VIM |
		MORPH_TUI_LIST_TAB | MORPH_TUI_LIST_DIGITS;
	morph_tui_list_layout(&select->menu.list, count, count);
	unsigned char checked = 0;
	int rc = morph_array_init(&select->checked, count, sizeof(checked));

	if (rc != 0)
		return rc;
	void *items = morph_array_push_n(&select->checked, count);

	if (!items) {
		morph_tui_select_cleanup(select);
		return -ENOMEM;
	}
	memset(items, 0, count);
	return 0;
}

int morph_tui_select_selected(const struct morph_tui_select *select, size_t index)
{
	if (!select || index >= select->menu.list.count)
		return 0;
	if (!select->multi)
		return index == select->menu.list.index;
	const unsigned char *checked = morph_array_get(&select->checked, index);

	return checked && *checked;
}

int morph_tui_select_update(struct morph_tui_select *select, int key)
{
	if (!select || !select->checked.nelts)
		return -EINVAL;
	int result = morph_tui_menu_update(&select->menu, key);

	if (select->menu.pasting || key == MORPH_TUI_KEY_PASTE_END)
		return MORPH_TUI_UNHANDLED;
	if (select->multi && key == ' ') {
		unsigned char *checked = morph_array_get(&select->checked,
							 select->menu.list.index);

		select->invalid = !*checked && select->selected_count == select->maximum;
		if (!select->invalid) {
			if (*checked)
				select->selected_count--;
			else
				select->selected_count++;
			*checked = (unsigned char)!*checked;
		}
		return MORPH_TUI_CHANGED;
	}
	if (result == MORPH_TUI_SUBMIT && select->multi) {
		select->invalid = select->selected_count < select->minimum ||
			select->selected_count > select->maximum;
		if (select->invalid)
			return MORPH_TUI_CHANGED;
	}
	if (result == MORPH_TUI_CHANGED)
		select->invalid = 0;
	return result;
}

void morph_tui_select_cleanup(struct morph_tui_select *select)
{
	if (!select)
		return;
	morph_array_cleanup(&select->checked);
	*select = (struct morph_tui_select){0};
}
