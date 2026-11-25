#include "sapi/cli/components/workspace_sidebar.h"

#include "sapi/cli/components/diff_browser.h"
#include "sapi/cli/components/file_browser.h"
#include <nb/err.h>
#include <nothing/tui/controls/button.h>
#include <nothing/tui/controls/tab_bar.h>
#include <nothing/tui/input.h>
#include <nothing/tui/keymap.h>
#include <nothing/tui/views/stack_view.h>

struct cli_workspace_sidebar {
	struct nt_stack_view base;
	struct nt_view *tabs, *close, *files, *diff;
	int selected, syncing;
};

int cli_workspace_sidebar_focused(const struct nt_view *view)
{
	if (nt_view_is_focused(view))
		return 1;

	for (size_t i = 0; i < nt_view_child_count(view); i++) {
		if (cli_workspace_sidebar_focused(nt_view_child(view, i)))
			return 1;
	}

	return 0;
}

int cli_workspace_sidebar_focus(struct nt_view *view)
{
	struct cli_workspace_sidebar *sidebar = (struct cli_workspace_sidebar *)view;
	return sidebar->selected ? cli_diff_browser_focus(sidebar->diff)
				 : cli_file_browser_focus(sidebar->files);
}

int cli_workspace_sidebar_sync(struct nt_view *view)
{
	struct cli_workspace_sidebar *sidebar = (struct cli_workspace_sidebar *)view;
	int rc = cli_file_browser_sync(sidebar->files);
	if (!rc)
		rc = cli_diff_browser_sync(sidebar->diff);

	if (!rc)
		rc = nt_view_set_hidden(sidebar->files, sidebar->selected != 0);

	if (!rc)
		rc = nt_view_set_hidden(sidebar->diff, sidebar->selected == 0);

	return rc;
}

static int cli_workspace_sidebar_action(void *data, const struct nt_view_event *event)
{
	struct cli_workspace_sidebar *sidebar = data;
	if (sidebar->syncing)
		return 0;

	if ((event->target == sidebar->files || event->target == sidebar->diff) &&
	    (event->type == NT_VIEW_EVENT_CANCEL || event->type == NT_VIEW_EVENT_OPEN))
		return nt_view_emit(&sidebar->base.base, event->type, event->index, event->text);

	if (event->target == sidebar->close && event->type == NT_VIEW_EVENT_ACTIVATE)
		return nt_view_emit(&sidebar->base.base, NT_VIEW_EVENT_CANCEL, 0, NULL);

	if (event->target != sidebar->tabs || event->type != NT_VIEW_EVENT_CHANGED)
		return 0;

	sidebar->selected = (int)nt_tab_bar_selected(sidebar->tabs);
	int rc = cli_workspace_sidebar_sync(&sidebar->base.base);
	if (!rc)
		rc = cli_workspace_sidebar_focus(&sidebar->base.base);

	return rc ? rc : NT_VIEW_EVENT_HANDLED;
}

static int cli_workspace_sidebar_capture(struct nt_view *view, const struct nt_event *event)
{
	struct cli_workspace_sidebar *sidebar = (struct cli_workspace_sidebar *)view;
	if (event->type != NT_EVENT_KEY || event->pasting || event->action == NT_KEY_ACTION_RELEASE)
		return 0;

	unsigned mods = event->modifiers & ~(unsigned)(NT_MOD_CAPS_LOCK | NT_MOD_NUM_LOCK);
	if (mods == NT_MOD_ALT && (event->key == '1' || event->key == '2')) {
		int rc = nt_tab_bar_select(sidebar->tabs, event->key == '1' ? 0u : 1u);
		return rc < 0 ? rc : 1;
	}

	return 0;
}

struct nt_view *cli_workspace_sidebar_add(struct nt_builder *builder, struct nt_view *parent,
					  struct cli_files_model *model, const char *workdir,
					  int styled)
{
	static const struct nt_view_class type = {.superclass = &nt_stack_view_class,
						  .instance_size =
							  sizeof(struct cli_workspace_sidebar),
						  .kind = "cli_workspace_sidebar",
						  .capture_event = cli_workspace_sidebar_capture};
	struct cli_workspace_sidebar *sidebar = (struct cli_workspace_sidebar *)nt_view_add(
		builder, parent, &type,
		&(struct nt_view_options){.width = NT_FILL, .height = NT_FILL});
	if (!sidebar)
		return NULL;

	struct nt_view *root = &sidebar->base.base;
	struct nt_view *bar = nt_row_add(builder, root, 1, &(struct nt_view_options){.height = 1});
	static const char *const tabs[] = {"Files", "Diff"};
	sidebar->tabs = nt_tab_bar_add(builder, bar, tabs, 2, 0, 0,
				       &(struct nt_view_options){.width = NT_FILL});
	sidebar->close = nt_button_add(builder, bar, "x", 0, NULL);
	sidebar->files = cli_file_browser_add(builder, root, model, workdir, styled);
	sidebar->diff = cli_diff_browser_add(builder, root, model, workdir, styled);
	static const struct nt_key_binding bindings[] = {
		{.key = '1', .modifiers = NT_MOD_ALT, .description = "Files sidebar tab"},
		{.key = '2', .modifiers = NT_MOD_ALT, .description = "Diff sidebar tab"}};
	static const struct nt_keymap map = {bindings, NT_ARRAY_SIZE(bindings)};
	if (!nt_builder_error(builder)) {
		nt_builder_fail(builder, nt_view_set_help_label(root, "Sidebar"));
		nt_builder_fail(builder, nt_view_set_keymap(root, &map));
		nt_builder_fail(builder, cli_workspace_sidebar_sync(root));
		if (!nt_view_on(root, NT_VIEW_EVENT_ACTION, cli_workspace_sidebar_action, sidebar))
			nt_builder_fail(builder, NB_ERR_NOMEM);
	}

	return root;
}

int cli_workspace_sidebar_open(struct nt_view *view, const char *path, int line)
{
	struct cli_workspace_sidebar *sidebar = (struct cli_workspace_sidebar *)view;
	int rc = nt_tab_bar_select(sidebar->tabs, 0);
	if (rc < 0)
		return rc;

	return cli_file_browser_open(sidebar->files, path, line);
}

int cli_workspace_sidebar_list(struct nt_view *view)
{
	struct cli_workspace_sidebar *sidebar = (struct cli_workspace_sidebar *)view;
	return sidebar->selected ? cli_diff_browser_focus(sidebar->diff)
				 : cli_file_browser_list(sidebar->files);
}

void cli_workspace_sidebar_reset(struct nt_view *view)
{
	struct cli_workspace_sidebar *sidebar = (struct cli_workspace_sidebar *)view;
	sidebar->syncing = 1;
	cli_file_browser_reset(sidebar->files);
	cli_diff_browser_reset(sidebar->diff);
	sidebar->selected = 0;
	(void)nt_tab_bar_select(sidebar->tabs, 0);
	sidebar->syncing = 0;
	(void)cli_workspace_sidebar_sync(view);
}
