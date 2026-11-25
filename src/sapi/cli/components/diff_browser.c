#include "sapi/cli/components/diff_browser.h"

#include "sapi/cli/components/shortcut_hint.h"
#include "sapi/cli/models/files.h"
#include "sapi/cli/presentation/diff.h"
#include "sapi/cli/services/git_diff.h"
#include "terminal/text.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nothing/tui/controls/button.h>
#include <nothing/tui/controls/list_view.h>
#include <nothing/tui/controls/tab_bar.h>
#include <nothing/tui/input.h>
#include <nothing/tui/keymap.h>
#include <nothing/tui/shortcut.h>
#include <nothing/tui/theme.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/stack_view.h>
#include <nothing/tui/views/text_view.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cli_diff_browser_source {
	struct nt_view *page, *back, *heading, *metadata, *previous, *next;
	struct nt_view *list, *empty, *document, *footer;
	struct cli_git_diff_result *inventory;
	struct cli_git_diff_job *job;
	struct nt_list_groups groups;
	const char *group_names[3];
	nb_vec labels, hunks;
	size_t selected;
	int viewing, loaded, previewing;
};

struct cli_diff_browser {
	struct nt_stack_view base;
	struct cli_files_model *model;
	const char *workdir;
	struct nt_view *sources, *refresh;
	struct cli_diff_browser_source source[2];
	uint64_t revision;
	int active, styled, syncing;
};

static void cli_diff_browser_labels_clear(struct cli_diff_browser_source *source)
{
	for (size_t i = 0; i < source->labels.len; i++)
		free(*(char **)nb_vec_at(&source->labels, i));

	nb_vec_clear(&source->labels);
}

static const char *cli_diff_browser_row(void *data, size_t index)
{
	const struct cli_diff_browser_source *source = data;
	const char *const *label = nb_vec_at(&source->labels, index);
	return label ? *label : "";
}

static int cli_diff_browser_row_draw(void *data, struct nt_canvas *canvas, size_t index,
				     size_t group, int selected)
{
	struct cli_diff_browser_source *source = data;
	if (index == SIZE_MAX)
		return nt_canvas_text(canvas, 0, 0, source->group_names[group], NT_STYLE_BOLD);

	return nt_canvas_text(canvas, 0, 0, cli_diff_browser_row(source, index),
			      selected ? NT_STYLE_SELECTED : 0);
}

static int cli_diff_browser_rule(struct nt_view *view, struct nt_canvas *canvas)
{
	(void)view;
	for (int i = 0; i < canvas->width; i++) {
		int rc = nt_canvas_text(canvas, i, 0, "─", NT_STYLE_DIM);
		if (rc)
			return rc;
	}

	return NB_OK;
}

static void cli_diff_browser_add_rule(struct nt_builder *builder, struct nt_view *parent)
{
	static const struct nt_view_class type = {.superclass = &nt_view_base_class,
						  .instance_size = sizeof(struct nt_view),
						  .kind = "cli_diff_browser_rule",
						  .draw = cli_diff_browser_rule};
	(void)nt_view_add(builder, parent, &type, &(struct nt_view_options){.height = 1});
}

static int cli_diff_browser_visibility(struct cli_diff_browser *browser)
{
	for (size_t i = 0; i < 2; i++) {
		struct cli_diff_browser_source *source = &browser->source[i];
		struct {
			struct nt_view *view;
			int hidden;
		} changes[] = {{source->page, browser->active != (int)i},
			       {source->back, !source->viewing},
			       {source->previous, !source->viewing},
			       {source->next, !source->viewing},
			       {source->list, source->viewing || !source->labels.len},
			       {source->empty, source->viewing || source->labels.len != 0},
			       {source->document, !source->viewing}};
		for (size_t j = 0; j < sizeof(changes) / sizeof(changes[0]); j++) {
			int rc = nt_view_set_hidden(changes[j].view, changes[j].hidden);
			if (rc)
				return rc;
		}

		int rc = cli_shortcut_hint_set_text(
			source->footer, source->viewing
						? "PgUp/PgDn scroll · N/P hunk · Ctrl+F find\nR "
						  "refresh · Esc back · Ctrl+O chat"
						: "Up/Down select · Enter view · R refresh\nAlt+S "
						  "Session · Alt+G Git · Esc close");
		if (rc)
			return rc;
	}

	return NB_OK;
}

static int cli_diff_browser_session(struct cli_diff_browser *browser)
{
	if (browser->revision == browser->model->revision)
		return NB_OK;

	struct cli_diff_browser_source *source = &browser->source[0];
	cli_diff_browser_labels_clear(source);
	for (size_t i = browser->model->changes.len; i > 0; i--) {
		const struct cli_files_change *change = nb_vec_at(&browser->model->changes, i - 1);
		const char *path = change->path;
		size_t length = strlen(browser->workdir);
		if (!strncmp(path, browser->workdir, length) && path[length] == '/')
			path += length + 1;

		char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
			path, strlen(path), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
		nb_buf label NB_CLEANUP(nb_buf_cleanup) = {0};
		if (!safe || !nb_buf_printf(&label, "%zu  %c %s", i, change->action, safe))
			return NB_ERR_NOMEM;

		char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(nb_buf_cstr(&label));
		if (!copy || !nb_vec_push_copy(&source->labels, &copy))
			return NB_ERR_NOMEM;

		copy = NULL;
	}

	int rc = nt_list_view_set_source(source->list, source->labels.len, cli_diff_browser_row,
					 source);
	nb_buf heading NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!nb_buf_printf(&heading, "Session patches (%zu)%s", source->labels.len,
			   browser->model->truncated ? " · limit reached" : ""))
		return NB_ERR_NOMEM;

	if (!rc && !source->viewing)
		rc = nt_label_set_text(source->heading, nb_buf_cstr(&heading));

	if (!rc)
		browser->revision = browser->model->revision;

	return rc;
}

static int cli_diff_browser_refresh(struct cli_diff_browser *browser)
{
	struct cli_diff_browser_source *source = &browser->source[browser->active];
	if (!browser->active) {
		browser->revision = UINT64_MAX;
		return cli_diff_browser_sync(&browser->base.base);
	}

	cli_git_diff_stop(&source->job);
	const struct cli_git_diff_entry *entry =
		source->viewing && source->inventory
			? nb_vec_at(&source->inventory->entries, source->selected)
			: NULL;
	source->previewing = entry != NULL;
	const char *directory = entry ? nb_buf_cstr(&source->inventory->root) : browser->workdir;
	int rc = cli_git_diff_start(directory, entry, &source->job);
	if (!rc)
		rc = nt_label_set_text(source->metadata, "Loading Git changes...");

	return rc;
}

static int cli_diff_browser_open(struct cli_diff_browser *browser, size_t index)
{
	struct cli_diff_browser_source *source = &browser->source[browser->active];
	if (index >= source->labels.len)
		return NB_OK;

	source->selected = browser->active ? index : browser->model->changes.len - index - 1;
	source->viewing = 1;
	int rc = NB_OK;
	if (browser->active) {
		const struct cli_git_diff_entry *entry =
			nb_vec_at(&source->inventory->entries, index);
		rc = nt_label_set_text(source->heading, nb_buf_cstr(&entry->label));
		if (!rc)
			rc = nt_text_view_set_text(source->document, "Loading diff...");

		if (!rc)
			rc = cli_diff_browser_refresh(browser);
	} else {
		const struct cli_files_change *change =
			nb_vec_at(&browser->model->changes, source->selected);
		rc = nt_label_set_text(source->heading, cli_diff_browser_row(source, index));
		if (!rc)
			rc = cli_diff_present(source->document, change->patch, &source->hunks,
					      browser->styled);
	}

	if (!rc)
		rc = cli_diff_browser_visibility(browser);

	if (!rc)
		rc = nt_view_focus(source->document);

	return rc;
}

static int cli_diff_browser_inventory(struct cli_diff_browser *browser,
				      struct cli_git_diff_result **owner)
{
	struct cli_diff_browser_source *source = &browser->source[1];
	(void)nt_list_view_set_groups(source->list, NULL);
	nt_list_groups_deinit(&source->groups);
	cli_diff_browser_labels_clear(source);
	cli_git_diff_result_cleanup(&source->inventory);
	source->inventory = *owner;
	*owner = NULL;
	source->loaded = 1;
	struct cli_git_diff_result *result = source->inventory;
	size_t counts[3] = {0};
	static const char *const names[] = {"Unstaged", "Staged", "Untracked"};
	for (size_t i = 0; !result->status && i < result->entries.len; i++) {
		const struct cli_git_diff_entry *entry = nb_vec_at(&result->entries, i);
		char *label NB_CLEANUP(nb_charp_cleanup) = strdup(nb_buf_cstr(&entry->label));
		if (!label || !nb_vec_push_copy(&source->labels, &label))
			return NB_ERR_NOMEM;

		label = NULL;
		counts[entry->kind]++;
	}

	size_t nonempty[3] = {0}, groups = 0;
	for (size_t i = 0; i < 3; i++) {
		if (!counts[i])
			continue;

		source->group_names[groups] = names[i];
		nonempty[groups++] = counts[i];
	}

	int rc = nt_list_view_set_source(source->list, source->labels.len, cli_diff_browser_row,
					 source);
	if (!rc && groups)
		rc = nt_list_groups_init(&source->groups, nonempty, groups);

	if (!rc && groups)
		rc = nt_list_view_set_groups(source->list, &source->groups);

	nb_buf metadata NB_CLEANUP(nb_buf_cleanup) = {0};
	if (result->status) {
		nb_buf_printf(&metadata, "Git unavailable: %s", nb_status_str(result->status));
		rc = rc ? rc
			: nt_label_set_text(
				  source->empty,
				  result->error.len
					  ? nb_buf_cstr(&result->error)
					  : "No Git repository is available for this directory.");
	} else {
		char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
			nb_buf_cstr(&result->branch), result->branch.len,
			MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
		if (!safe)
			return NB_ERR_NOMEM;

		nb_buf_printf(&metadata, "%s · %zu changes", safe, source->labels.len);
		rc = rc ? rc : nt_label_set_text(source->empty, "Working tree clean.");
	}

	if (!nb_buf_ok(&metadata))
		return nb_buf_status(&metadata);

	if (!rc)
		rc = nt_label_set_text(source->metadata, nb_buf_cstr(&metadata));

	if (!rc)
		rc = nt_label_set_text(source->heading, "Git changes");

	return rc;
}

int cli_diff_browser_sync(struct nt_view *view)
{
	struct cli_diff_browser *browser = (struct cli_diff_browser *)view;
	int rc = cli_diff_browser_session(browser);
	int focus_list = 0;
	struct cli_diff_browser_source *source = &browser->source[1];
	if (!rc && source->job) {
		struct cli_git_diff_result *result NB_CLEANUP(cli_git_diff_result_cleanup) = NULL;
		rc = cli_git_diff_collect(&source->job, &result);
		if (rc == NB_ERR_AGAIN)
			rc = NB_OK;
		else if (!rc && source->previewing) {
			if (result->truncated)
				nb_buf_append_cstr(&result->patch,
						   "\nDiff output truncated at 1 MiB.\n");

			const char *text =
				result->status ? (result->error.len ? nb_buf_cstr(&result->error)
								    : nb_status_str(result->status))
				: result->patch.len
					? nb_buf_cstr(&result->patch)
					: "This change is no longer present. Refresh the list.";
			rc = cli_diff_present(source->document, text, &source->hunks,
					      browser->styled);
			const struct cli_git_diff_entry *entry =
				source->inventory
					? nb_vec_at(&source->inventory->entries, source->selected)
					: NULL;
			if (!rc)
				rc = nt_label_set_text(
					source->metadata,
					entry && entry->kind == CLI_GIT_DIFF_STAGED
						? "Staged · index vs HEAD"
					: entry && entry->kind == CLI_GIT_DIFF_UNTRACKED
						? "Untracked · new file"
						: "Unstaged · working tree vs index");
		} else if (!rc) {
			focus_list = browser->active == 1 && nt_view_is_focused(browser->sources);
			rc = cli_diff_browser_inventory(browser, &result);
		}
	}

	if (!rc)
		rc = cli_diff_browser_visibility(browser);

	if (!rc && focus_list && source->labels.len)
		rc = nt_view_focus(source->list);

	return rc;
}

int cli_diff_browser_focus(struct nt_view *view)
{
	struct cli_diff_browser *browser = (struct cli_diff_browser *)view;
	struct cli_diff_browser_source *source = &browser->source[browser->active];
	return nt_view_focus(source->viewing	  ? source->document
			     : source->labels.len ? source->list
						  : browser->sources);
}

static int cli_diff_browser_source_select(struct cli_diff_browser *browser, int active)
{
	browser->active = active;
	int rc = NB_OK;
	if (active && !browser->source[1].loaded && !browser->source[1].job)
		rc = cli_diff_browser_refresh(browser);

	if (!rc)
		rc = cli_diff_browser_sync(&browser->base.base);

	return rc ? rc : cli_diff_browser_focus(&browser->base.base);
}

static int cli_diff_browser_action(void *data, const struct nt_view_event *event)
{
	struct cli_diff_browser *browser = data;
	if (browser->syncing)
		return 0;

	if (event->target == browser->sources && event->type == NT_VIEW_EVENT_CHANGED) {
		int rc = cli_diff_browser_source_select(browser,
							(int)nt_tab_bar_selected(browser->sources));
		return rc ? rc : NT_VIEW_EVENT_HANDLED;
	}

	if (event->type != NT_VIEW_EVENT_ACTIVATE)
		return 0;

	struct cli_diff_browser_source *source = &browser->source[browser->active];
	int rc = NB_OK;
	if (event->target == source->list)
		rc = cli_diff_browser_open(browser, event->index);
	else if (event->target == browser->refresh)
		rc = cli_diff_browser_refresh(browser);
	else if (event->target == source->back) {
		source->viewing = 0;
		if (browser->active)
			(void)nt_label_set_text(source->heading, "Git changes");

		browser->revision = UINT64_MAX;
		rc = cli_diff_browser_sync(&browser->base.base);
		if (!rc)
			rc = cli_diff_browser_focus(&browser->base.base);
	} else if (event->target == source->previous || event->target == source->next) {
		size_t index = browser->active ? source->selected
					       : browser->model->changes.len - source->selected - 1;
		size_t count = source->labels.len;
		if (count)
			rc = cli_diff_browser_open(browser, event->target == source->previous
								    ? (index + count - 1) % count
								    : (index + 1) % count);
	} else
		return 0;

	return rc ? rc : NT_VIEW_EVENT_HANDLED;
}

static int cli_diff_browser_capture(struct nt_view *view, const struct nt_event *event)
{
	struct cli_diff_browser *browser = (struct cli_diff_browser *)view;
	if (event->pasting || event->type != NT_EVENT_KEY || event->action == NT_KEY_ACTION_RELEASE)
		return 0;

	unsigned mods = event->modifiers & ~(unsigned)(NT_MOD_CAPS_LOCK | NT_MOD_NUM_LOCK);
	if (mods == NT_MOD_ALT && (event->key == 's' || event->key == 'g')) {
		int rc = nt_tab_bar_select(browser->sources, event->key == 'g' ? 1u : 0u);
		return rc < 0 ? rc : 1;
	}

	struct cli_diff_browser_source *source = &browser->source[browser->active];
	if (source->viewing && nt_view_is_focused(source->document) &&
	    (event->key == 6 || (event->key == 'f' && mods == NT_MOD_CTRL))) {
		struct nt_event find = *event;
		find.key = 'f';
		find.modifiers = nt_shortcut_primary_modifier();
		return nt_view_class_methods(source->document)
			->handle_event(source->document, &find);
	}

	return 0;
}

static int cli_diff_browser_key(struct nt_view *view, const struct nt_event *event)
{
	struct cli_diff_browser *browser = (struct cli_diff_browser *)view;
	if (event->pasting || event->action == NT_KEY_ACTION_RELEASE)
		return 0;

	unsigned mods =
		event->modifiers & ~(unsigned)(NT_MOD_SHIFT | NT_MOD_CAPS_LOCK | NT_MOD_NUM_LOCK);
	if (mods)
		return 0;

	int key = event->key;
	if (event->type == NT_EVENT_TEXT && event->text_length == 1)
		key = (unsigned char)event->text[0];
	else if (event->type != NT_EVENT_KEY || (event->flags & NT_EVENT_TEXT_FOLLOWS))
		return 0;

	struct cli_diff_browser_source *source = &browser->source[browser->active];
	int rc = NB_OK;
	if (key == 'r' || key == 'R')
		rc = cli_diff_browser_refresh(browser);
	else if ((key == 'n' || key == 'N' || key == 'p' || key == 'P') && source->viewing) {
		size_t cursor = nt_text_view_cursor(source->document), target = cursor;
		int next = key == 'n' || key == 'N';
		for (size_t i = 0; i < source->hunks.len; i++) {
			size_t offset = *(size_t *)nb_vec_at(&source->hunks, i);
			if (next && offset > cursor) {
				target = offset;
				break;
			}

			if (!next && offset < cursor)
				target = offset;
		}

		rc = nt_text_view_set_cursor(source->document, target);
	} else if ((key == 27 || key == NT_KEY_ESCAPE) && source->viewing) {
		source->viewing = 0;
		if (browser->active)
			(void)nt_label_set_text(source->heading, "Git changes");

		browser->revision = UINT64_MAX;
		rc = cli_diff_browser_sync(view);
		if (!rc)
			rc = cli_diff_browser_focus(view);
	} else if (key == 27 || key == NT_KEY_ESCAPE || key == 3)
		return nt_view_emit(view, NT_VIEW_EVENT_CANCEL, 0, NULL);
	else
		return 0;

	return rc ? rc : 1;
}

static const struct nt_keymap *cli_diff_browser_keymap(const struct nt_view *view)
{
	(void)view;
	static const struct nt_key_binding bindings[] = {
		{.key = 's', .modifiers = NT_MOD_ALT, .description = "Session diffs"},
		{.key = 'g', .modifiers = NT_MOD_ALT, .description = "Git diffs"},
		{.key = 'r', .help = "R", .description = "Refresh changes"},
		{.key = 27, .description = "Back to changes or close sidebar"},
		{.key = 'n', .help = "N", .description = "Next diff hunk"},
		{.key = 'p', .help = "P", .description = "Previous diff hunk"},
		{.key = 'f', .modifiers = NT_MOD_CTRL, .description = "Find in diff"},
	};
	static const struct nt_keymap list = {bindings, 4};
	static const struct nt_keymap detail = {bindings, NT_ARRAY_SIZE(bindings)};
	const struct cli_diff_browser *browser = (const struct cli_diff_browser *)view;
	return browser->source[browser->active].viewing ? &detail : &list;
}

static void cli_diff_browser_prepare_deinit(struct nt_view *view)
{
	struct cli_diff_browser *browser = (struct cli_diff_browser *)view;
	for (size_t i = 0; i < 2; i++)
		cli_git_diff_stop(&browser->source[i].job);
}

static void cli_diff_browser_destroy(struct nt_view *view)
{
	struct cli_diff_browser *browser = (struct cli_diff_browser *)view;
	for (size_t i = 0; i < 2; i++) {
		struct cli_diff_browser_source *source = &browser->source[i];
		cli_git_diff_result_cleanup(&source->inventory);
		cli_diff_browser_labels_clear(source);
		nb_vec_cleanup(&source->labels);
		nb_vec_cleanup(&source->hunks);
		nt_list_groups_deinit(&source->groups);
	}
}

struct nt_view *cli_diff_browser_add(struct nt_builder *builder, struct nt_view *parent,
				     struct cli_files_model *model, const char *workdir, int styled)
{
	static const struct nt_view_class type = {.superclass = &nt_stack_view_class,
						  .instance_size = sizeof(struct cli_diff_browser),
						  .kind = "cli_diff_browser",
						  .capture_event = cli_diff_browser_capture,
						  .handle_event = cli_diff_browser_key,
						  .keymap = cli_diff_browser_keymap,
						  .prepare_deinit = cli_diff_browser_prepare_deinit,
						  .deinit = cli_diff_browser_destroy};
	struct cli_diff_browser *browser = (struct cli_diff_browser *)nt_view_add(
		builder, parent, &type,
		&(struct nt_view_options){.width = NT_FILL, .height = NT_FILL});
	if (!browser)
		return NULL;

	browser->model = model;
	browser->workdir = workdir;
	browser->styled = styled;
	browser->revision = UINT64_MAX;
	struct nt_view *root = &browser->base.base;
	nt_builder_fail(builder, nt_view_set_help_label(root, "Diff"));
	struct nt_view *bar = nt_row_add(builder, root, 1, &(struct nt_view_options){.height = 1});
	static const char *const sources[] = {"Session", "Git"};
	browser->sources = nt_tab_bar_add(builder, bar, sources, 2, 0, 0,
					  &(struct nt_view_options){.width = NT_FILL});
	browser->refresh = nt_button_add(builder, bar, "Refresh", 0, NULL);
	for (size_t i = 0; i < 2; i++) {
		struct cli_diff_browser_source *source = &browser->source[i];
		nb_vec_init(&source->labels, sizeof(char *));
		nb_vec_init(&source->hunks, sizeof(size_t));
		source->page = nt_column_add(
			builder, root, 0,
			&(struct nt_view_options){.width = NT_FILL, .height = NT_FILL});
		struct nt_view *toolbar = nt_row_add(builder, source->page, 1,
						     &(struct nt_view_options){.height = 1});
		source->back = nt_button_add(builder, toolbar, "< Changes", 0, NULL);
		source->heading =
			nt_label_add(builder, toolbar, i ? "Git changes" : "Session patches",
				     &(struct nt_view_options){.width = NT_FILL});
		source->previous = nt_button_add(builder, toolbar, "Prev", 0, NULL);
		source->next = nt_button_add(builder, toolbar, "Next", 0, NULL);
		source->metadata =
			nt_label_add(builder, source->page,
				     i ? "Select Git to load changes."
				       : "Recorded tool patches · current session · newest first",
				     &(struct nt_view_options){.height = 1});
		cli_diff_browser_add_rule(builder, source->page);
		struct nt_view *body =
			nt_view_add(builder, source->page, &nt_view_base_class,
				    &(struct nt_view_options){.width = NT_FILL,
							      .height = NT_FILL,
							      .padding = {.left = 1, .right = 1}});
		source->list = nt_list_view_add(
			builder, body, NULL, 0, 0,
			&(struct nt_view_options){.width = NT_FILL, .height = NT_FILL});
		source->document = nt_text_view_add(
			builder, body, "",
			&(struct nt_view_options){.width = NT_FILL, .height = NT_FILL});
		source->empty =
			nt_label_add(builder, body,
				     i ? "Select Git to load changes."
				       : "No recorded patches in this session.\n\nShell commands "
					 "and external edits are not tracked here.",
				     &(struct nt_view_options){.width = NT_FILL});
		cli_diff_browser_add_rule(builder, source->page);
		source->footer = cli_shortcut_hint_add(
			builder, source->page, "",
			&(struct nt_view_options){.width = NT_FILL, .height = NT_AUTO});
		if (!nt_builder_error(builder)) {
			nt_builder_fail(builder, nt_text_view_set_wrap_mode(source->document,
									    NT_TEXT_WRAP_NONE));
			nt_builder_fail(builder,
					nt_list_view_set_activate_on_click(source->list, 1));
			nt_builder_fail(builder,
					nt_label_set_wrap_mode(source->heading, NT_TEXT_WRAP_NONE));
			nt_builder_fail(builder, nt_label_set_wrap_mode(source->metadata,
									NT_TEXT_WRAP_NONE));
			nt_builder_fail(builder, nt_view_set_selectable(source->metadata, 0));
			nt_builder_fail(builder, nt_view_set_selectable(source->empty, 0));
			if (i)
				nt_builder_fail(
					builder,
					nt_list_view_set_row_renderer(
						source->list, cli_diff_browser_row_draw, source));
		}
	}

	if (!nt_builder_error(builder)) {
		nt_builder_fail(builder, cli_diff_browser_sync(root));
		if (!nt_view_on(root, NT_VIEW_EVENT_ACTION, cli_diff_browser_action, browser))
			nt_builder_fail(builder, NB_ERR_NOMEM);
	}

	return root;
}

void cli_diff_browser_reset(struct nt_view *view)
{
	struct cli_diff_browser *browser = (struct cli_diff_browser *)view;
	browser->syncing = 1;
	for (size_t i = 0; i < 2; i++) {
		struct cli_diff_browser_source *source = &browser->source[i];
		cli_git_diff_stop(&source->job);
		(void)nt_list_view_set_groups(source->list, NULL);
		(void)nt_list_view_set_source(source->list, 0, cli_diff_browser_row, source);
		nt_list_groups_deinit(&source->groups);
		cli_git_diff_result_cleanup(&source->inventory);
		cli_diff_browser_labels_clear(source);
		nb_vec_clear(&source->hunks);
		(void)nt_text_view_set_text(source->document, "");
		source->selected = 0;
		source->viewing = source->loaded = source->previewing = 0;
	}

	browser->active = 0;
	browser->revision = UINT64_MAX;
	(void)nt_tab_bar_select(browser->sources, 0);
	browser->syncing = 0;
	(void)cli_diff_browser_sync(view);
}
