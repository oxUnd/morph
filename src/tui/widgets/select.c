#include <morph/tui/widgets/select.h>
#include "nb/err.h"
#include <string.h>

int morph_tui_widget_select_init(struct morph_tui_widget_select *select, size_t count, int multi,
				 size_t minimum, size_t maximum)
{
	if (!select || !count || minimum > count || maximum > count ||
	    (maximum && maximum < minimum) || (!multi && (minimum > 1 || maximum > 1)))
		return NB_ERR_INVAL;
	*select = (struct morph_tui_widget_select){0};
	select->multi = multi != 0;
	select->selected_count = multi ? 0 : 1;
	select->minimum = minimum;
	select->maximum = multi ? (maximum ? maximum : count) : 1;
	select->menu.list.flags = MORPH_TUI_WIDGET_LIST_WRAP | MORPH_TUI_WIDGET_LIST_VIM |
				  MORPH_TUI_WIDGET_LIST_TAB | MORPH_TUI_WIDGET_LIST_DIGITS;
	morph_tui_widget_list_state_layout(&select->menu.list, count, count);
	unsigned char checked = 0;
	int rc = (nb_vec_init(&select->checked, sizeof(checked)),
		  nb_vec_reserve(&select->checked, count) ? 0 : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;
	void *items = nb_vec_push_n(&select->checked, count);

	if (!items) {
		morph_tui_widget_select_cleanup(select);
		return NB_ERR_NOMEM;
	}
	memset(items, 0, count);
	return 0;
}

int morph_tui_widget_select_selected(const struct morph_tui_widget_select *select, size_t index)
{
	if (!select || index >= select->menu.list.count)
		return 0;
	if (!select->multi)
		return index == select->menu.list.index;
	const unsigned char *checked = nb_vec_at(&select->checked, index);

	return checked && *checked;
}

int morph_tui_widget_select_update(struct morph_tui_widget_select *select, int key)
{
	if (!select || !select->checked.len)
		return NB_ERR_INVAL;
	int result = morph_tui_widget_menu_state_update(&select->menu, key);

	if (select->menu.pasting || key == MORPH_TUI_KEY_PASTE_END)
		return MORPH_TUI_WIDGET_UNHANDLED;
	if (select->multi && key == ' ') {
		unsigned char *checked = nb_vec_at(&select->checked, select->menu.list.index);

		select->invalid = !*checked && select->selected_count == select->maximum;
		if (!select->invalid) {
			if (*checked)
				select->selected_count--;
			else
				select->selected_count++;
			*checked = (unsigned char)!*checked;
		}
		return MORPH_TUI_WIDGET_CHANGED;
	}
	if (result == MORPH_TUI_WIDGET_SUBMIT && select->multi) {
		select->invalid = select->selected_count < select->minimum ||
				  select->selected_count > select->maximum;
		if (select->invalid)
			return MORPH_TUI_WIDGET_CHANGED;
	}
	if (result == MORPH_TUI_WIDGET_CHANGED)
		select->invalid = 0;
	return result;
}

void morph_tui_widget_select_cleanup(struct morph_tui_widget_select *select)
{
	if (!select)
		return;
	nb_vec_cleanup(&select->checked);
	*select = (struct morph_tui_widget_select){0};
}
