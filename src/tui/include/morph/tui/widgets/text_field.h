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
/* Copies text; the caller retains the input. */
int morph_tui_widget_text_field_set_text_copy(struct morph_tui_view *, const char *text);
/* Multiline fields wrap to their content width. Enter submits; Ctrl+J and
 * pasted newlines insert a newline. Prompt is copied and sanitized. */
int morph_tui_widget_text_field_set_multiline(struct morph_tui_view *, int enabled);
int morph_tui_widget_text_field_set_prompt_copy(struct morph_tui_view *, const char *prompt);
int morph_tui_widget_text_field_set_prompt_style(struct morph_tui_view *, struct morph_tui_text_style);
int morph_tui_widget_text_field_set_completion(struct morph_tui_view *, int enabled);
/* Inserts sanitized text at the cursor, preserving the rest of the draft. */
int morph_tui_widget_text_field_insert_text_copy(struct morph_tui_view *, const char *text);

#ifdef __cplusplus
}
#endif
#endif
