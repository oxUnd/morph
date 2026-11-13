#ifndef MORPH_TUI_WIDGET_H
#define MORPH_TUI_WIDGET_H

#include <morph/tui/view.h>

#ifdef __cplusplus
extern "C" {
#endif

/* All built-in controls embed this base first. Custom controls use the same
 * view methods, focus routing, ownership, and semantic action dispatch. */
struct morph_tui_widget {
	struct morph_tui_view view;
};

enum morph_tui_widget_action_type {
	MORPH_TUI_WIDGET_ACTION_ACTIVATE,
	MORPH_TUI_WIDGET_ACTION_CHANGED,
	MORPH_TUI_WIDGET_ACTION_SUBMIT,
	MORPH_TUI_WIDGET_ACTION_COMPLETE
};

struct morph_tui_widget_action {
	struct morph_tui_view *sender;
	enum morph_tui_widget_action_type type;
	int tag;
	size_t index;
	/* Borrowed for the synchronous action callback only. */
	const char *text;
};

/* Adds a controller-owned widget, borrowing ops for the node's lifetime.
 * The result is borrowed until controller destruction/build rollback. */
struct morph_tui_widget *morph_tui_widget_add_borrow(struct morph_tui_builder *builder,
						     struct morph_tui_view *parent, size_t size,
						     const struct morph_tui_view_ops *ops,
						     const struct morph_tui_view_options *options);
int morph_tui_widget_set_tag(struct morph_tui_view *view, int tag);
/* A binding overrides controller action dispatch. Its context is borrowed
 * until replacement or view destruction; callbacks return zero or an error. */
int morph_tui_widget_set_action_borrow(struct morph_tui_view *view,
				       int (*action)(void *,
						     const struct morph_tui_widget_action *),
				       void *data);
int morph_tui_widget_emit(struct morph_tui_view *view, enum morph_tui_widget_action_type type,
			  size_t index, const char *text);

#ifdef __cplusplus
}
#endif
#endif
