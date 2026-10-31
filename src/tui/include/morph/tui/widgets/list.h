#ifndef MORPH_TUI_WIDGET_LIST_H
#define MORPH_TUI_WIDGET_LIST_H

#include <morph/tui/terminal/input.h>
#include <stddef.h>
#include <morph/tui/widgets/base.h>

#ifdef __cplusplus
extern "C" {
#endif

/* State components own interaction, while the screen chooses presentation. */
enum morph_tui_widget_list_flags {
	MORPH_TUI_WIDGET_LIST_WRAP = 1,
	MORPH_TUI_WIDGET_LIST_VIM = 2,
	MORPH_TUI_WIDGET_LIST_TAB = 4,
	MORPH_TUI_WIDGET_LIST_DIGITS = 8,
	MORPH_TUI_WIDGET_LIST_HORIZONTAL_PAGES = 16,
	MORPH_TUI_WIDGET_LIST_PAGED = 32
};

struct morph_tui_widget_list {
	size_t count;
	size_t index;
	size_t first;
	size_t page;
	unsigned flags;
};

void morph_tui_widget_list_layout(struct morph_tui_widget_list *list, size_t count, size_t page);
int morph_tui_widget_list_update(struct morph_tui_widget_list *list, int key);
size_t morph_tui_widget_scroll_update(size_t offset, size_t count, size_t page, int key);

/* A menu handles paste isolation, wrap navigation and confirmation together. */
enum morph_tui_widget_result {
	MORPH_TUI_WIDGET_UNHANDLED,
	MORPH_TUI_WIDGET_CHANGED,
	MORPH_TUI_WIDGET_SUBMIT,
	MORPH_TUI_WIDGET_BACK,
	MORPH_TUI_WIDGET_CANCEL
};

struct morph_tui_widget_menu {
	struct morph_tui_widget_list list;
	int pasting;
};

int morph_tui_widget_menu_update(struct morph_tui_widget_menu *menu, int key);

/* Retained list control: copied items, built-in drawing, scrolling, focus,
 * invalidation, and ACTIVATE/CHANGED actions. Constructors join builder scope. */
struct morph_tui_core_view *
morph_tui_widget_list_view_create(struct morph_tui_core_builder *,
				  struct morph_tui_core_view *parent, const char *const *items,
				  size_t count, int tag,
				  const struct morph_tui_core_view_options *options);
int morph_tui_widget_list_view_set_items(struct morph_tui_core_view *, const char *const *items,
					 size_t count);
size_t morph_tui_widget_list_view_selected(const struct morph_tui_core_view *);
int morph_tui_widget_list_view_select(struct morph_tui_core_view *, size_t index);
int morph_tui_widget_list_view_set_navigation(struct morph_tui_core_view *, unsigned flags);

/* Data sources are borrowed until replaced or controller destruction. The
 * callback's text need only survive until the next callback invocation. */
typedef const char *(*morph_tui_widget_list_source_fn)(void *data, size_t index);
int morph_tui_widget_list_view_set_source(struct morph_tui_core_view *, size_t count,
					  morph_tui_widget_list_source_fn text, void *data);

#ifdef __cplusplus
}
#endif
#endif
