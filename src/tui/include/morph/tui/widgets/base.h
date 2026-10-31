#ifndef MORPH_TUI_WIDGET_BASE_H
#define MORPH_TUI_WIDGET_BASE_H

#include <morph/tui/core/view.h>

#ifdef __cplusplus
extern "C" {
#endif

/* All built-in controls embed this base first. Custom controls use the same
 * view methods, focus routing, ownership, and semantic action dispatch. */
struct morph_tui_widget_base {
	struct morph_tui_core_view view;
};

enum morph_tui_widget_action_type {
	MORPH_TUI_WIDGET_ACTION_ACTIVATE,
	MORPH_TUI_WIDGET_ACTION_CHANGED,
	MORPH_TUI_WIDGET_ACTION_SUBMIT
};

struct morph_tui_widget_action {
	struct morph_tui_core_view *sender;
	enum morph_tui_widget_action_type type;
	int tag;
	size_t index;
	/* Borrowed for the synchronous action callback only. */
	const char *text;
};

struct morph_tui_widget_base *
morph_tui_widget_base_create(struct morph_tui_core_builder *builder,
			     struct morph_tui_core_view *parent, size_t size,
			     const struct morph_tui_core_view_ops *ops,
			     const struct morph_tui_core_view_options *options);
int morph_tui_widget_base_set_tag(struct morph_tui_core_view *view, int tag);
/* A binding overrides controller action dispatch. Its context is borrowed
 * until replacement or view destruction; callbacks return zero or an error. */
int morph_tui_widget_base_set_action(struct morph_tui_core_view *view,
				     int (*action)(void *, const struct morph_tui_widget_action *),
				     void *data);
int morph_tui_widget_base_emit(struct morph_tui_core_view *view,
			       enum morph_tui_widget_action_type type, size_t index,
			       const char *text);

#ifdef __cplusplus
}
#endif
#endif
