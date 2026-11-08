#ifndef MORPH_TUI_WIDGET_SELECT_H
#define MORPH_TUI_WIDGET_SELECT_H

#include <morph/tui/widgets/list.h>
#include "util/array.h"
#include <signal.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct morph_cancel_token;

/* Select owns check state independently of labels and presentation.
 * Zero-initialize, init once, and cleanup on every exit. A zero maximum means
 * all items in multi mode. Invalid confirmation keeps the component open. */
struct morph_tui_widget_select {
	struct morph_tui_widget_menu_state menu;
	morph_array_t checked;
	size_t selected_count;
	size_t minimum;
	size_t maximum;
	int multi;
	int invalid;
};

int morph_tui_widget_select_init(struct morph_tui_widget_select *select, size_t count, int multi,
				 size_t minimum, size_t maximum);
int morph_tui_widget_select_update(struct morph_tui_widget_select *select, int key);
int morph_tui_widget_select_selected(const struct morph_tui_widget_select *select, size_t index);
void morph_tui_widget_select_cleanup(struct morph_tui_widget_select *select);

/* Inline selector presentation. items are borrowed until run returns.
 * With allow_custom, init state with one extra item reserved at the end;
 * items contains only the regular labels. custom receives an owned answer
 * if selected, or NULL. Without allow_custom, custom may be NULL.
 * The caller owns signals and maps cancellation to application actions. */
struct morph_tui_widget_select_options {
	const char *const *items;
	const volatile sig_atomic_t *interrupted;
	int color;
	int allow_custom;
	/* Maximum custom answer bytes, excluding NUL; zero defaults to 1 MiB. */
	size_t custom_max_bytes;
	/* Zero disables automatic expiry. Any answer key disables it permanently. */
	int timeout_seconds;
	const char *hint;
	const struct morph_cancel_token *cancel_token;
};

int morph_tui_widget_select_run(struct morph_tui_widget_select *select,
				const struct morph_tui_widget_select_options *options, FILE *input,
				FILE *output, char **custom);

#ifdef __cplusplus
}
#endif
#endif
