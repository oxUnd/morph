#include "sapi/cli/adapters/input/focus.h"

#include <nb/err.h>
#include <nothing/tui/application.h>
#include <nothing/tui/input.h>
#include <nothing/tui/views/text_view.h>
#include <nothing/tui/window.h>
#include <string.h>

struct cli_focus_path {
	struct nt_view *scope, *pane;
};

struct cli_focus_candidates {
	struct nt_view *first;
	size_t count;
};

static int cli_focus_path_find(struct nt_view *view, struct nt_view *focused,
	struct cli_focus_path *path)
{
	if (!view || nt_view_is_hidden(view) || !nt_view_is_enabled(view))
		return 0;

	int found = view == focused;
	for (size_t i = 0; !found && i < nt_view_child_count(view); i++) {
		struct nt_view *child = nt_view_child(view, i);
		found = cli_focus_path_find(child, focused, path);
		if (found && !path->scope && !path->pane &&
		    !strcmp(nt_view_kind(view), "split_view"))
			path->pane = child;
	}

	if (found && !path->scope && nt_view_is_focus_scope(view))
		path->scope = view;

	return found;
}

static int cli_focus_is_editor(const struct nt_view *view)
{
	if (nt_view_is_read_only(view))
		return 0;

	const char *kind = nt_view_kind(view);
	return !strcmp(kind, "text_field") ||
		(!strcmp(kind, "text_view") && nt_text_view_is_editable(view));
}

static void cli_focus_collect(struct nt_view *view,
	struct cli_focus_candidates *candidates)
{
	if (!view || candidates->count > 1 || nt_view_is_hidden(view) ||
	    !nt_view_is_enabled(view) || nt_view_is_read_only(view))
		return;

	struct nt_view_rect frame = nt_view_frame(view);
	if (cli_focus_is_editor(view) && frame.width > 0 && frame.height > 0) {
		if (!candidates->first)
			candidates->first = view;

		candidates->count++;
		return;
	}

	for (size_t i = 0; candidates->count < 2 && i < nt_view_child_count(view); i++)
		cli_focus_collect(nt_view_child(view, i), candidates);
}

static struct nt_view *cli_focus_target(struct nt_window *window)
{
	struct nt_view *root = nt_window_root_view(window);
	struct nt_view *focused = nt_window_focused_view(window);
	if (focused && cli_focus_is_editor(focused))
		return NULL;

	struct cli_focus_path path = {0};
	cli_focus_path_find(root, focused, &path);
	struct cli_focus_candidates candidates = {0};
	cli_focus_collect(path.scope ? path.scope : root, &candidates);
	if (candidates.count == 1)
		return candidates.first;

	if (!path.pane)
		return NULL;

	candidates = (struct cli_focus_candidates){0};
	cli_focus_collect(path.pane, &candidates);
	return candidates.count == 1 ? candidates.first : NULL;
}

int cli_input_focus_forward(struct nt_application *app, const struct nt_event *event)
{
	if (!app || !event)
		return NB_ERR_INVAL;

	unsigned shortcuts = NT_MOD_CTRL | NT_MOD_ALT | NT_MOD_SUPER | NT_MOD_HYPER | NT_MOD_META;
	if (event->action == NT_KEY_ACTION_RELEASE || (event->modifiers & shortcuts) ||
	    nt_application_transition_pending(app))
		return 0;

	if (event->type == NT_EVENT_TEXT || event->type == NT_EVENT_PASTE) {
		if (!event->text || !event->text_length)
			return 0;
	} else {
		if (event->type != NT_EVENT_KEY || event->pasting)
			return 0;

		switch (event->key) {
		case ' ':
		case 8:
		case 0x7F:
		case NT_KEY_BACKSPACE:
		case NT_KEY_DELETE:
		case NT_KEY_LEFT:
		case NT_KEY_RIGHT:
		case NT_KEY_HOME:
		case NT_KEY_END:
			break;
		default:
			return 0;
		}
	}

	struct nt_window *window = nt_application_window(app);
	struct nt_view *target = window ? cli_focus_target(window) : NULL;
	if (!target)
		return 0;

	int rc = nt_window_focus_view(window, target);
	if (rc == NB_ERR_STATE)
		return 0;

	if (rc)
		return rc;

	rc = nt_view_class_methods(target)->handle_event(target, event);
	return rc < 0 ? rc : 1;
}

static int cli_focus_unhandled(void *data, const struct nt_event *event)
{
	if (event->type != NT_EVENT_TEXT && event->type != NT_EVENT_PASTE)
		return 0;

	return cli_input_focus_forward(data, event);
}

int cli_input_focus_install(struct nt_application *app)
{
	struct nt_window *window = app ? nt_application_window(app) : NULL;
	return window ? nt_window_set_event_handler(window, cli_focus_unhandled, app)
		: NB_ERR_INVAL;
}
