#ifndef MORPH_TUI_LIST_H
#define MORPH_TUI_LIST_H

#include <morph/tui/input.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* State components own interaction, while the screen chooses presentation. */
enum morph_tui_list_flags {
	MORPH_TUI_LIST_WRAP = 1,
	MORPH_TUI_LIST_VIM = 2,
	MORPH_TUI_LIST_TAB = 4,
	MORPH_TUI_LIST_DIGITS = 8,
	MORPH_TUI_LIST_HORIZONTAL_PAGES = 16,
	MORPH_TUI_LIST_PAGED = 32
};

struct morph_tui_list {
	size_t count;
	size_t index;
	size_t first;
	size_t page;
	unsigned flags;
};

void morph_tui_list_layout(struct morph_tui_list *list, size_t count, size_t page);
int morph_tui_list_update(struct morph_tui_list *list, int key);
size_t morph_tui_scroll_update(size_t offset, size_t count, size_t page, int key);

/* A menu handles paste isolation, wrap navigation and confirmation together. */
enum morph_tui_result {
	MORPH_TUI_UNHANDLED,
	MORPH_TUI_CHANGED,
	MORPH_TUI_SUBMIT,
	MORPH_TUI_BACK,
	MORPH_TUI_CANCEL
};

struct morph_tui_menu {
	struct morph_tui_list list;
	int pasting;
};

int morph_tui_menu_update(struct morph_tui_menu *menu, int key);

#ifdef __cplusplus
}
#endif
#endif
