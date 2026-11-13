#ifndef MORPH_TUI_WIDGET_TEXT_FIELD_H
#define MORPH_TUI_WIDGET_TEXT_FIELD_H

#include <morph/tui/widget.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Adds a controller-owned node, copying initial text. The node is borrowed
 * until controller destruction/build rollback. */
struct morph_tui_view *
morph_tui_widget_text_field_add_borrow(struct morph_tui_builder *, struct morph_tui_view *parent,
				       const char *text, size_t limit, int tag,
				       const struct morph_tui_view_options *options);
/* Borrowed text, invalidated by an edit/setter or controller destruction. */
const char *morph_tui_widget_text_field_text_borrow(const struct morph_tui_view *);
/* Cursor offsets and edit ranges are UTF-8 byte offsets. Programmatic edits
 * preserve undo history, sanitize replacement text and leave surrounding text. */
size_t morph_tui_widget_text_field_cursor(const struct morph_tui_view *);
int morph_tui_widget_text_field_set_cursor(struct morph_tui_view *, size_t offset);
int morph_tui_widget_text_field_replace_range_copy(struct morph_tui_view *, size_t start,
						 size_t end, const char *replacement);
/* Copies text; the caller retains the input. */
int morph_tui_widget_text_field_set_text_copy(struct morph_tui_view *, const char *text);
/* Multiline fields wrap to their content width. Enter submits; Ctrl+J and
 * pasted newlines insert a newline. Prompt is copied and sanitized. */
int morph_tui_widget_text_field_set_multiline(struct morph_tui_view *, int enabled);
int morph_tui_widget_text_field_set_prompt_copy(struct morph_tui_view *, const char *prompt);
/* Continuation rows align after the prompt by default. Disable to start them
 * at column zero while retaining the prompt only on the first physical row. */
int morph_tui_widget_text_field_set_prompt_hanging_indent(struct morph_tui_view *, int enabled);
int morph_tui_widget_text_field_set_prompt_style(struct morph_tui_view *,
						 struct morph_tui_text_style);
/* A borrowed, optional decorator changes the style of visible text at a UTF-8
 * byte offset. It runs during drawing on the UI owner and must not mutate
 * views. The style starts with the field's resolved inherited style. */
int morph_tui_widget_text_field_set_decoration_borrow(
	struct morph_tui_view *,
	int (*decorate)(void *, const char *text, size_t offset, struct morph_tui_text_style *),
	void *data);
/* Resolve an atomic edit token containing the cursor (backward includes its
 * right edge, forward includes its left edge). Return one with [start,end),
 * zero for ordinary editing, or a negative error. The borrowed callback must
 * not mutate views. Arrow movement and deletion then handle the entire token. */
int morph_tui_widget_text_field_set_atomic_range_resolver_borrow(
	struct morph_tui_view *,
	int (*resolve)(void *, const char *, size_t cursor, int backward,
		       size_t *start, size_t *end), void *data);
int morph_tui_widget_text_field_set_completion(struct morph_tui_view *, int enabled);
/* Inserts sanitized text at the cursor, preserving the rest of the draft. */
int morph_tui_widget_text_field_insert_text_copy(struct morph_tui_view *, const char *text);

#ifdef __cplusplus
}
#endif
#endif
