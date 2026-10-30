#ifndef MORPH_TUI_COMPONENTS_H
#define MORPH_TUI_COMPONENTS_H

#include <morph/tui.h>
#include "util/array.h"
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-based origin and nonnegative extents. Empty regions are valid. */
struct morph_tui_rect {
	int x;
	int y;
	int width;
	int height;
};

struct morph_tui_rect morph_tui_rect_inset(struct morph_tui_rect area,
					 int horizontal, int vertical);
struct morph_tui_rect morph_tui_rect_take_top(struct morph_tui_rect *area, int height);
struct morph_tui_rect morph_tui_rect_take_left(struct morph_tui_rect *area, int width);
int morph_tui_region_cell(morph_buf_t *commands, struct morph_tui_rect area,
		    int row, int column, int width, const char *text,
		    const char *style);

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

/* Select owns check state independently of labels and presentation.
 * Zero-initialize, init once, and cleanup on every exit. A zero maximum means
 * all items in multi mode. Invalid confirmation keeps the component open. */
struct morph_tui_select {
	struct morph_tui_menu menu;
	morph_array_t checked;
	size_t selected_count;
	size_t minimum;
	size_t maximum;
	int multi;
	int invalid;
};

int morph_tui_select_init(struct morph_tui_select *select, size_t count,
			  int multi, size_t minimum, size_t maximum);
int morph_tui_select_update(struct morph_tui_select *select, int key);
int morph_tui_select_selected(const struct morph_tui_select *select, size_t index);
void morph_tui_select_cleanup(struct morph_tui_select *select);

/* Inline selector presentation. items are borrowed until run returns.
 * With allow_custom, init state with one extra item reserved at the end;
 * items contains only the regular labels. custom receives an owned answer
 * if selected, or NULL. Without allow_custom, custom may be NULL.
 * The caller owns signals and maps cancellation to application actions. */
struct morph_tui_select_options {
	const char *const *items;
	const volatile sig_atomic_t *interrupted;
	int color;
	int allow_custom;
	/* Zero disables automatic expiry. Any answer key disables it permanently. */
	int timeout_seconds;
	const char *hint;
	const struct morph_cancel_token *cancel_token;
};

int morph_tui_select_run(struct morph_tui_select *select,
			 const struct morph_tui_select_options *options,
			 FILE *input, FILE *output, char **custom);

int morph_tui_card_border(morph_buf_t *line, int width, int bottom);
int morph_tui_card_row(morph_buf_t *line, int width, const char *text);

/* UTF-8 byte input is accumulated without hand-written encoding logic.
 * limit includes the final NUL. Cleanup wipes owned input, including secrets. */
struct morph_tui_editor {
	morph_buf_t text;
	size_t cursor;
	size_t limit;
	int secret;
	int pasting;
	int overflow;
};

int morph_tui_editor_init(struct morph_tui_editor *editor, const char *text,
		    size_t limit, int secret);
int morph_tui_editor_update(struct morph_tui_editor *editor, int key);
int morph_tui_editor_valid(const struct morph_tui_editor *editor);
int morph_tui_editor_view(struct morph_tui_editor *editor, morph_buf_t *text,
		    size_t width, size_t *cursor_column);
void morph_tui_editor_cleanup(struct morph_tui_editor *editor);

/* Text is borrowed until the next wrap/cleanup; lines contain byte spans. */
struct morph_tui_text_line {
	size_t offset;
	size_t length;
};

struct morph_tui_text_view {
	const char *text;
	morph_array_t lines;
};

int morph_tui_text_view_wrap(struct morph_tui_text_view *view, const char *text, size_t width);
void morph_tui_text_view_cleanup(struct morph_tui_text_view *view);

/* Screen ownership: one session, one decoder, one event/update/draw loop.
 * Signal handlers remain application-owned; interrupted is an optional flag. */
enum morph_tui_event_type {
	MORPH_TUI_EVENT_TICK,
	MORPH_TUI_EVENT_KEY,
	MORPH_TUI_EVENT_RESIZE,
	MORPH_TUI_EVENT_EOF
};

struct morph_tui_event {
	enum morph_tui_event_type type;
	int key;
	int pasting;
};

struct morph_tui_screen {
	struct morph_tui_session session;
	struct morph_tui_input input;
	struct morph_tui_rect area;
	const volatile sig_atomic_t *interrupted;
	int output_fd;
	int pasting;
	int dirty;
};

enum morph_tui_screen_result {
	MORPH_TUI_SCREEN_CONTINUE,
	MORPH_TUI_SCREEN_REDRAW,
	MORPH_TUI_SCREEN_CLOSE
};

struct morph_tui_screen_ops {
	int (*update)(void *data, const struct morph_tui_event *event);
	int (*draw)(void *data, const struct morph_tui_screen *screen);
};

int morph_tui_screen_open(struct morph_tui_screen *screen, FILE *input, FILE *output, int flags);
int morph_tui_screen_poll(struct morph_tui_screen *screen,
			  struct morph_tui_event *event, int timeout_ms);
int morph_tui_screen_run(struct morph_tui_screen *screen,
			 const struct morph_tui_screen_ops *ops, void *data);
int morph_tui_screen_close(struct morph_tui_screen *screen);

#ifdef __cplusplus
}
#endif
#endif
