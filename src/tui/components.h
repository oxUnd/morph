#ifndef MORPH_TUI_COMPONENTS_H
#define MORPH_TUI_COMPONENTS_H

#include "tui/tui.h"
#include "util/array.h"
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-based origin and nonnegative extents. Empty regions are valid. */
struct tui_rect {
	int x;
	int y;
	int width;
	int height;
};

struct tui_rect tui_rect_inset(struct tui_rect area, int horizontal, int vertical);
struct tui_rect tui_rect_take_top(struct tui_rect *area, int height);
struct tui_rect tui_rect_take_left(struct tui_rect *area, int width);
int tui_region_cell(morph_buf_t *commands, struct tui_rect area,
		    int row, int column, int width, const char *text,
		    const char *style);

/* State components own interaction, while the screen chooses presentation. */
enum tui_list_flags {
	TUI_LIST_WRAP = 1,
	TUI_LIST_VIM = 2,
	TUI_LIST_TAB = 4,
	TUI_LIST_DIGITS = 8,
	TUI_LIST_HORIZONTAL_PAGES = 16,
	TUI_LIST_PAGED = 32
};

struct tui_list {
	size_t count;
	size_t index;
	size_t first;
	size_t page;
	unsigned flags;
};

void tui_list_layout(struct tui_list *list, size_t count, size_t page);
int tui_list_update(struct tui_list *list, int key);
size_t tui_scroll_update(size_t offset, size_t count, size_t page, int key);

/* A menu handles paste isolation, wrap navigation and confirmation together. */
enum tui_result {
	TUI_UNHANDLED,
	TUI_CHANGED,
	TUI_SUBMIT,
	TUI_BACK,
	TUI_CANCEL
};

struct tui_menu {
	struct tui_list list;
	int pasting;
};

int tui_menu_update(struct tui_menu *menu, int key);
int tui_card_border(morph_buf_t *line, int width, int bottom);
int tui_card_row(morph_buf_t *line, int width, const char *text);

/* UTF-8 byte input is accumulated without hand-written encoding logic.
 * limit includes the final NUL. Cleanup wipes owned input, including secrets. */
struct tui_editor {
	morph_buf_t text;
	size_t cursor;
	size_t limit;
	int secret;
	int pasting;
	int overflow;
};

int tui_editor_init(struct tui_editor *editor, const char *text,
		    size_t limit, int secret);
int tui_editor_update(struct tui_editor *editor, int key);
int tui_editor_valid(const struct tui_editor *editor);
int tui_editor_view(struct tui_editor *editor, morph_buf_t *text,
		    size_t width, size_t *cursor_column);
void tui_editor_cleanup(struct tui_editor *editor);

/* Text is borrowed until the next wrap/cleanup; lines contain byte spans. */
struct tui_text_line {
	size_t offset;
	size_t length;
};

struct tui_text_view {
	const char *text;
	morph_array_t lines;
};

int tui_text_view_wrap(struct tui_text_view *view, const char *text, size_t width);
void tui_text_view_cleanup(struct tui_text_view *view);

/* Screen ownership: one session, one decoder, one event/update/draw loop.
 * Signal handlers remain application-owned; interrupted is an optional flag. */
enum tui_event_type {
	TUI_EVENT_TICK,
	TUI_EVENT_KEY,
	TUI_EVENT_RESIZE,
	TUI_EVENT_EOF
};

struct tui_event {
	enum tui_event_type type;
	int key;
	int pasting;
};

struct tui_screen {
	struct tui_session session;
	struct tui_input input;
	struct tui_rect area;
	const volatile sig_atomic_t *interrupted;
	int output_fd;
	int pasting;
	int dirty;
};

enum tui_screen_result {
	TUI_SCREEN_CONTINUE,
	TUI_SCREEN_REDRAW,
	TUI_SCREEN_CLOSE
};

struct tui_screen_ops {
	int (*update)(void *data, const struct tui_event *event);
	int (*draw)(void *data, const struct tui_screen *screen);
};

int tui_screen_open(struct tui_screen *screen, FILE *input, FILE *output, int flags);
int tui_screen_poll(struct tui_screen *screen, struct tui_event *event, int timeout_ms);
int tui_screen_run(struct tui_screen *screen, const struct tui_screen_ops *ops, void *data);
int tui_screen_close(struct tui_screen *screen);

#ifdef __cplusplus
}
#endif
#endif
