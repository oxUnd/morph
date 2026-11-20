#include "sapi/cli/components/file_browser.h"

#include "sapi/cli/components/file_viewer.h"
#include "sapi/cli/models/files.h"
#include "sapi/cli/presentation/style.h"
#include "terminal/text.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/file.h>
#include <nb/vec.h>
#include <nothing/tui/controls/button.h>
#include <nothing/tui/controls/list_view.h>
#include <nothing/tui/controls/tab_bar.h>
#include <nothing/tui/input.h>
#include <nothing/tui/terminal/text.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/scroll_view.h>
#include <nothing/tui/views/stack_view.h>
#include <limits.h>
#include <string.h>

struct cli_file_browser {
	struct nt_stack_view base;
	struct nt_builder *builder;
	struct cli_files_model *model;
	const char *workdir;
	struct nt_view *toolbar, *back, *heading, *open, *tab_scroll, *tabs;
	struct nt_view *body, *list, *empty, *pages, *footer;
	nb_vec viewers, tab_widths;
	size_t selected, open_count;
	uint64_t revision;
	int styled, viewing, syncing, tabs_dirty, tab_columns, reveal;
};

static struct nt_view *cli_file_browser_current(const struct cli_file_browser *browser)
{
	struct nt_view *const *viewer = nb_vec_at(&browser->viewers, browser->selected);
	return viewer ? *viewer : NULL;
}

static const char *cli_file_browser_row(void *data, size_t index)
{
	struct cli_file_browser *browser = data;
	const struct cli_files_entry *entry = nb_vec_at(&browser->model->entries, index);
	return entry ? entry->path : "";
}

static int cli_file_browser_draw_row(void *data, struct nt_canvas *canvas, size_t index,
	size_t group, int selected)
{
	struct cli_file_browser *browser = data;
	(void)group;
	const struct cli_files_entry *entry = nb_vec_at(&browser->model->entries, index);
	if (!entry || canvas->width < 3)
		return NB_OK;

	struct nt_text_style name_style = canvas->style, muted = canvas->style;
	if (browser->styled) {
		name_style.foreground = (struct nt_color){NT_COLOR_INDEXED,
			selected ? 15u : CLI_INPUT_ULTRA_INDEX};
		muted.foreground = (struct nt_color){NT_COLOR_INDEXED, selected ? 252u : 245u};
		if (selected) {
			name_style.background = muted.background = (struct nt_color){
				NT_COLOR_INDEXED, CLI_FILE_SELECTION_BACKGROUND_INDEX};
			name_style.attributes = NT_STYLE_BOLD;
		}
	} else if (selected)
		name_style.attributes = muted.attributes = NT_STYLE_SELECTED | NT_STYLE_BOLD;

	int rc = nt_canvas_fill_styled(canvas, &muted);
	const char *path = entry->path;
	size_t length = strlen(browser->workdir);
	if (!strncmp(path, browser->workdir, length) && path[length] == '/')
		path += length + 1;

	char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(path,
		strlen(path), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;

	path = safe;
	const char *basename = nb_path_basename(path);
	nb_buf directory NB_CLEANUP(nb_buf_cleanup) = {0};
	if (basename > path) {
		if (!nb_buf_append(&directory, path, (size_t)(basename - path)))
			return nb_buf_status(&directory);
	} else if (!nb_buf_append_cstr(&directory, "./"))
		return nb_buf_status(&directory);

	size_t available = (size_t)(canvas->width - 2);
	size_t directory_width = nt_terminal_display_width_ansi(nb_buf_cstr(&directory)) + 3;
	size_t reserve = directory_width < available / 2 ? directory_width : available / 2;
	char name[PATH_MAX], folder[PATH_MAX];
	size_t name_width = morph_terminal_copy_ellipsized_display_width(name, sizeof(name),
		basename, available - reserve, 0);
	size_t remaining = available - name_width;
	if (!rc)
		rc = nt_canvas_text_styled(canvas, 0, 0, "- ", &muted);

	if (!rc)
		rc = nt_canvas_text_styled(canvas, 2, 0, name, &name_style);

	if (!rc && remaining > 3) {
		(void)morph_terminal_copy_ellipsized_display_width(folder, sizeof(folder),
			nb_buf_cstr(&directory), remaining - 3, 1);
		nb_buf suffix NB_CLEANUP(nb_buf_cleanup) = {0};
		if (!nb_buf_printf(&suffix, " (%s)", folder))
			return nb_buf_status(&suffix);

		rc = nt_canvas_text_styled(canvas, 2 + (int)name_width, 0,
			nb_buf_cstr(&suffix), &muted);
	}

	return rc;
}

static int cli_file_browser_rule(struct nt_view *view, struct nt_canvas *canvas)
{
	(void)view;
	for (int column = 0; column < canvas->width; column++) {
		int rc = nt_canvas_text(canvas, column, 0, "─", NT_STYLE_DIM);
		if (rc)
			return rc;
	}

	return NB_OK;
}

static void cli_file_browser_add_rule(struct cli_file_browser *browser)
{
	static const struct nt_view_class type = {
		.superclass = &nt_view_base_class,
		.instance_size = sizeof(struct nt_view),
		.kind = "cli_file_browser_rule",
		.draw = cli_file_browser_rule,
	};
	struct nt_view *rule = nt_view_add(browser->builder, &browser->base.base, &type,
		&(struct nt_view_options){.height = 1});
	if (rule)
		nt_builder_fail(browser->builder, nt_view_set_selectable(rule, 0));
}

static int cli_file_browser_draw(struct nt_view *view, struct nt_canvas *canvas)
{
	struct cli_file_browser *browser = (struct cli_file_browser *)view;
	if (!browser->styled)
		return NB_OK;

	struct nt_view_rect content = nt_view_content_rect(view);
	struct nt_view *bands[] = {browser->toolbar, browser->tab_scroll, browser->footer};
	for (size_t i = 0; i < sizeof(bands) / sizeof(bands[0]); i++) {
		if (nt_view_is_hidden(bands[i]))
			continue;

		struct nt_view_rect frame = nt_view_frame(bands[i]);
		struct nt_canvas band;
		nt_canvas_subcanvas(canvas, frame.x - content.x, frame.y - content.y,
			frame.width, frame.height, &band);
		struct nt_text_style style = canvas->style;
		style.background = (struct nt_color){NT_COLOR_INDEXED, CLI_INPUT_BACKGROUND_INDEX};
		int rc = nt_canvas_fill_styled(&band, &style);
		if (rc)
			return rc;
	}

	return NB_OK;
}

static int cli_file_browser_tab_labels(struct cli_file_browser *browser)
{
	int columns = nt_view_content_rect(browser->tab_scroll).width;
	if (!browser->tabs_dirty && columns == browser->tab_columns)
		return NB_OK;

	size_t count = browser->viewers.len;
	nb_vec labels NB_CLEANUP(nb_vec_cleanup) = {0};
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_vec_init(&labels, sizeof(const char *));
	nb_vec_clear(&browser->tab_widths);
	size_t name_columns = columns > 8 && columns < 36 ? (size_t)(columns - 6) : 30u;
	for (size_t i = 0; i < count; i++) {
		struct nt_view *viewer = *(struct nt_view **)nb_vec_at(&browser->viewers, i);
		const char *basename = nb_path_basename(cli_file_viewer_path(viewer));
		char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(basename,
			strlen(basename), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
		if (!safe)
			return NB_ERR_NOMEM;

		char name[PATH_MAX];
		size_t width = morph_terminal_copy_ellipsized_display_width(name, sizeof(name),
			safe, name_columns, 0) + 4u;
		size_t *slot = nb_vec_push(&browser->tab_widths);
		if (!slot || !nb_buf_printf(&text, " %s × ", name) ||
		    !nb_buf_append_char(&text, '\0'))
			return NB_ERR_NOMEM;

		*slot = width;
	}

	const char *label = nb_buf_cstr(&text);
	for (size_t i = 0; i < count; i++) {
		const char **slot = nb_vec_push(&labels);
		if (!slot)
			return NB_ERR_NOMEM;

		*slot = label;
		label += strlen(label) + 1;
	}

	browser->syncing = 1;
	int rc = nt_tab_bar_set_items(browser->tabs, (const char *const *)labels.data, count);
	if (!rc && count) {
		rc = nt_tab_bar_select(browser->tabs, browser->selected);
		if (rc > 0)
			rc = NB_OK;
	}

	browser->syncing = 0;
	if (!rc) {
		browser->tabs_dirty = 0;
		browser->tab_columns = columns;
		browser->reveal = 1;
	}

	return rc;
}

static int cli_file_browser_reveal(struct cli_file_browser *browser)
{
	if (!browser->reveal || nt_view_is_hidden(browser->tab_scroll))
		return NB_OK;

	size_t start = 0;
	for (size_t i = 0; i < browser->selected; i++)
		start += *(size_t *)nb_vec_at(&browser->tab_widths, i) + 1u;

	const size_t *width = nb_vec_at(&browser->tab_widths, browser->selected);
	int columns = nt_view_content_rect(browser->tab_scroll).width;
	if (!width || columns <= 0 || start + *width > INT_MAX)
		return NB_OK;

	int offset = nt_scroll_view_horizontal_offset(browser->tab_scroll);
	if ((int)start < offset)
		offset = (int)start;
	else if ((int)(start + *width) > offset + columns)
		offset = (int)(start + *width) - columns;

	int rc = nt_scroll_view_set_horizontal_offset(browser->tab_scroll, offset);
	if (!rc && nt_scroll_view_horizontal_offset(browser->tab_scroll) == offset)
		browser->reveal = 0;

	return rc;
}

static int cli_file_browser_layout(struct nt_view *view)
{
	struct cli_file_browser *browser = (struct cli_file_browser *)view;
	const struct nt_view_class *super = nt_view_super_methods(view, view->type);
	int rc = super->layout(view);
	if (rc)
		return rc;

	if (browser->tab_columns != nt_view_content_rect(browser->tab_scroll).width) {
		rc = cli_file_browser_tab_labels(browser);
		if (!rc)
			rc = super->layout(view);
	}

	if (!rc)
		rc = cli_file_browser_reveal(browser);

	return rc;
}

static int cli_file_browser_show(struct cli_file_browser *browser, int viewing)
{
	browser->viewing = viewing && browser->viewers.len;
	int rc = cli_file_browser_sync(&browser->base.base);
	if (!rc)
		rc = cli_file_browser_focus(&browser->base.base);

	return rc;
}

static int cli_file_browser_close_tab(struct cli_file_browser *browser, size_t index)
{
	struct nt_view **viewer = nb_vec_at(&browser->viewers, index);
	if (!viewer)
		return NB_OK;

	/* Slots are framework-owned. Clear and reuse empty slots when tabs close. */
	for (size_t i = 0; i < nt_view_child_count(browser->pages); i++) {
		struct nt_view *slot = nt_view_child(browser->pages, i);
		if (nt_view_child(slot, 0) != *viewer)
			continue;

		int rc = nt_view_clear_children(slot);
		if (rc)
			return rc;

		(void)nt_view_set_hidden(slot, 1);
		break;
	}

	nb_vec_remove(&browser->viewers, index);
	if (index < browser->selected || browser->selected == browser->viewers.len) {
		if (browser->selected)
			browser->selected--;
	}

	browser->tabs_dirty = browser->reveal = 1;
	return cli_file_browser_show(browser, browser->viewers.len != 0);
}

static int cli_file_browser_action(void *data, const struct nt_view_event *event)
{
	struct cli_file_browser *browser = data;
	if (browser->syncing)
		return 0;

	if (event->type == NT_VIEW_EVENT_OPEN && event->target != &browser->base.base)
		return nt_view_emit(&browser->base.base, NT_VIEW_EVENT_OPEN, SIZE_MAX, event->text);

	int rc = NB_OK;
	if (event->target == browser->list && event->type == NT_VIEW_EVENT_ACTIVATE) {
		const struct cli_files_entry *entry = nb_vec_at(&browser->model->entries, event->index);
		return entry ? nt_view_emit(&browser->base.base, NT_VIEW_EVENT_OPEN,
			event->index, entry->path) : 0;
	}

	if (event->target == browser->back && event->type == NT_VIEW_EVENT_ACTIVATE)
		rc = cli_file_browser_show(browser, 0);
	else if (event->target == browser->open && event->type == NT_VIEW_EVENT_ACTIVATE)
		rc = cli_file_browser_show(browser, 1);
	else if (event->target == browser->tabs && event->type == NT_VIEW_EVENT_CHANGED) {
		browser->selected = nt_tab_bar_selected(browser->tabs);
		browser->reveal = 1;
		rc = cli_file_browser_show(browser, 1);
	} else
		return 0;

	return rc ? rc : NT_VIEW_EVENT_HANDLED;
}

static int cli_file_browser_tab_click(void *data, const struct nt_view_event *event)
{
	struct cli_file_browser *browser = data;
	if (!event->input || event->input->mouse.button != NT_MOUSE_LEFT)
		return 0;

	int column = event->input->mouse.x - nt_view_frame(browser->tabs).x;
	size_t offset = 0;
	for (size_t i = 0; i < browser->tab_widths.len; i++) {
		size_t width = *(size_t *)nb_vec_at(&browser->tab_widths, i);
		if (column >= 0 && (size_t)column == offset + width - 2u) {
			int rc = cli_file_browser_close_tab(browser, i);
			return rc ? rc : NT_VIEW_EVENT_STOP;
		}

		if (i == browser->selected && column >= 0 &&
		    (size_t)column >= offset && (size_t)column < offset + width) {
			int rc = cli_file_browser_focus(&browser->base.base);
			return rc ? rc : NT_VIEW_EVENT_STOP;
		}

		offset += width + 1u;
	}

	return 0;
}

static int cli_file_browser_capture(struct nt_view *view, const struct nt_event *event)
{
	struct cli_file_browser *browser = (struct cli_file_browser *)view;
	if (!browser->viewing || event->pasting || event->type != NT_EVENT_KEY ||
	    event->action == NT_KEY_ACTION_RELEASE)
		return 0;

	unsigned mods = event->modifiers & ~(unsigned)(NT_MOD_CAPS_LOCK | NT_MOD_NUM_LOCK);
	if (event->key == 23 || ((event->key == 'w' || event->key == 'W') && mods == NT_MOD_CTRL)) {
		int rc = cli_file_browser_close_tab(browser, browser->selected);
		return rc ? rc : 1;
	}

	int previous = (event->key == NT_KEY_PAGE_UP && mods == NT_MOD_CTRL) ||
		(event->key == NT_KEY_LEFT && mods == NT_MOD_ALT);
	int next = (event->key == NT_KEY_PAGE_DOWN && mods == NT_MOD_CTRL) ||
		(event->key == NT_KEY_RIGHT && mods == NT_MOD_ALT);
	if ((previous || next) && browser->viewers.len > 1) {
		size_t count = browser->viewers.len;
		size_t index = previous ? (browser->selected + count - 1) % count
			: (browser->selected + 1) % count;
		int rc = nt_tab_bar_select(browser->tabs, index);
		return rc < 0 ? rc : 1;
	}

	return 0;
}

static int cli_file_browser_key(struct nt_view *view, const struct nt_event *event)
{
	struct cli_file_browser *browser = (struct cli_file_browser *)view;
	if (event->pasting || event->type != NT_EVENT_KEY ||
	    event->action == NT_KEY_ACTION_RELEASE)
		return 0;

	if (event->key == 27 || event->key == NT_KEY_ESCAPE) {
		if (!browser->viewing)
			return nt_view_emit(view, NT_VIEW_EVENT_CANCEL, 0, NULL);

		int rc = cli_file_browser_show(browser, 0);
		return rc ? rc : 1;
	}

	return event->key == 3 ? nt_view_emit(view, NT_VIEW_EVENT_CANCEL, 0, NULL) : 0;
}

static void cli_file_browser_destroy(struct nt_view *view)
{
	struct cli_file_browser *browser = (struct cli_file_browser *)view;
	nb_vec_cleanup(&browser->viewers);
	nb_vec_cleanup(&browser->tab_widths);
}

struct nt_view *cli_file_browser_add(struct nt_builder *builder, struct nt_view *parent,
	struct cli_files_model *model, const char *workdir, int styled)
{
	static const struct nt_view_class type = {
		.superclass = &nt_stack_view_class,
		.instance_size = sizeof(struct cli_file_browser),
		.kind = "cli_file_browser",
		.layout = cli_file_browser_layout,
		.draw = cli_file_browser_draw,
		.capture_event = cli_file_browser_capture,
		.handle_event = cli_file_browser_key,
		.deinit = cli_file_browser_destroy,
	};
	struct cli_file_browser *browser = (struct cli_file_browser *)nt_view_add(builder,
		parent, &type, &(struct nt_view_options){.width = NT_FILL, .height = NT_FILL});
	if (!browser)
		return NULL;

	browser->builder = builder;
	browser->model = model;
	browser->workdir = workdir;
	browser->styled = styled;
	browser->revision = UINT64_MAX;
	browser->open_count = SIZE_MAX;
	browser->tabs_dirty = 1;
	nb_vec_init(&browser->viewers, sizeof(struct nt_view *));
	nb_vec_init(&browser->tab_widths, sizeof(size_t));
	struct nt_view *root = &browser->base.base;
	struct nt_view_options toolbar = {.height = 1};
	if (styled)
		toolbar.style.background = (struct nt_color){NT_COLOR_INDEXED,
			CLI_INPUT_BACKGROUND_INDEX};

	browser->toolbar = nt_row_add(builder, root, 1, &toolbar);
	browser->back = nt_button_add(builder, browser->toolbar, "< Files", 0, NULL);
	browser->heading = nt_label_add(builder, browser->toolbar, "Files",
		&(struct nt_view_options){.width = NT_FILL, .padding.left = 1});
	browser->open = nt_button_add(builder, browser->toolbar, "Open", 0, NULL);
	browser->tab_scroll = nt_scroll_view_add(builder, root, &toolbar);
	browser->tabs = nt_tab_bar_add(builder, browser->tab_scroll, NULL, 0, 0, 0, NULL);
	cli_file_browser_add_rule(browser);
	browser->body = nt_view_add(builder, root, &nt_view_base_class,
		&(struct nt_view_options){.height = NT_FILL, .padding = {.left = 1, .right = 1}});
	browser->list = nt_list_view_add(builder, browser->body, NULL, 0, 0,
		&(struct nt_view_options){.width = NT_FILL, .height = NT_FILL, .margin.top = 1});
	browser->empty = nt_label_add(builder, browser->body,
		"Files referenced, modified or generated in this conversation appear here.",
		&(struct nt_view_options){.width = NT_FILL, .margin.top = 1});
	browser->pages = nt_view_add(builder, browser->body, &nt_view_base_class,
		&(struct nt_view_options){.width = NT_FILL, .height = NT_FILL});
	cli_file_browser_add_rule(browser);
	struct nt_view_options footer = toolbar;
	footer.height = NT_AUTO;
	footer.padding.left = footer.padding.right = 1;
	browser->footer = nt_label_add(builder, root,
		"Up/Down select · Enter open\nCtrl+O chat · Esc close", &footer);
	if (!nt_builder_error(builder)) {
		nt_builder_fail(builder, nt_label_set_wrap_mode(browser->heading, NT_TEXT_WRAP_NONE));
		nt_builder_fail(builder, nt_view_set_selectable(browser->heading, 0));
		nt_builder_fail(builder, nt_view_set_selectable(browser->footer, 0));
		nt_builder_fail(builder, nt_view_set_selectable(browser->empty, 0));
		nt_builder_fail(builder, nt_view_set_focusable(browser->tab_scroll, 0));
		nt_builder_fail(builder, nt_scroll_view_set_scrollbars(browser->tab_scroll, 0));
		nt_builder_fail(builder, nt_list_view_set_row_renderer(browser->list,
			cli_file_browser_draw_row, browser));
		nt_builder_fail(builder, nt_list_view_set_activate_on_click(browser->list, 1));
		struct nt_text_style selected = {.attributes = NT_STYLE_BOLD | NT_STYLE_SELECTED};
		if (styled) {
			selected.attributes = NT_STYLE_BOLD;
			selected.foreground = (struct nt_color){NT_COLOR_INDEXED, 15};
			selected.background = (struct nt_color){NT_COLOR_INDEXED,
				CLI_FILE_SELECTION_BACKGROUND_INDEX};
		}

		nt_builder_fail(builder, nt_tab_bar_set_appearance(browser->tabs, 1, 0, selected));
		if (!nt_view_on(root, NT_VIEW_EVENT_ACTION, cli_file_browser_action, browser) ||
		    !nt_view_on_full(browser->tabs, NT_VIEW_EVENT_CLICK,
			&(struct nt_view_listener_options){.flags = NT_VIEW_LISTEN_CAPTURE,
				NT_VIEW_LISTENER(cli_file_browser_tab_click, browser)}))
			nt_builder_fail(builder, NB_ERR_NOMEM);
	}

	return root;
}

int cli_file_browser_sync(struct nt_view *view)
{
	struct cli_file_browser *browser = (struct cli_file_browser *)view;
	for (size_t i = 0; i < browser->viewers.len; i++) {
		int rc = cli_file_viewer_sync(*(struct nt_view **)nb_vec_at(&browser->viewers, i));
		if (rc)
			return rc;
	}

	int rc = NB_OK;
	if (browser->revision != browser->model->revision) {
		rc = nt_list_view_set_source(browser->list, browser->model->entries.len,
			cli_file_browser_row, browser);
		if (rc)
			return rc;

		browser->revision = browser->model->revision;
	}

	struct nt_view *current = cli_file_browser_current(browser);
	nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!browser->viewing)
		nb_buf_printf(&title, "Files (%zu)%s", browser->model->entries.len,
			browser->model->truncated ? " · limit reached" : "");
	else if (browser->viewers.len > 1)
		nb_buf_printf(&title, "%zu open", browser->viewers.len);
	else if (current)
		nb_buf_append_cstr(&title, nb_path_basename(cli_file_viewer_path(current)));

	if (!nb_buf_ok(&title))
		return nb_buf_status(&title);

	rc = nt_label_set_text(browser->heading, nb_buf_cstr(&title));
	if (!rc && browser->open_count != browser->viewers.len) {
		nb_buf open NB_CLEANUP(nb_buf_cleanup) = {0};
		if (!nb_buf_printf(&open, "Open: %zu", browser->viewers.len))
			return nb_buf_status(&open);

		rc = nt_button_set_label(browser->open, nb_buf_cstr(&open));
		if (!rc)
			browser->open_count = browser->viewers.len;
	}

	if (!rc)
		rc = nt_label_set_text(browser->footer, browser->viewing && current
			? cli_file_viewer_hint(current)
			: "Up/Down select · Enter open\nCtrl+O chat · Esc close");

	struct { struct nt_view *view; int hidden; } visibility[] = {
		{browser->back, !browser->viewing},
		{browser->open, browser->viewing || !browser->viewers.len},
		{browser->tab_scroll, !browser->viewing || browser->viewers.len < 2},
		{browser->list, browser->viewing},
		{browser->empty, browser->viewing || browser->model->entries.len != 0},
		{browser->pages, !browser->viewing},
	};
	for (size_t i = 0; !rc && i < sizeof(visibility) / sizeof(visibility[0]); i++)
		rc = nt_view_set_hidden(visibility[i].view, visibility[i].hidden);

	for (size_t i = 0; !rc && i < nt_view_child_count(browser->pages); i++) {
		struct nt_view *slot = nt_view_child(browser->pages, i);
		rc = nt_view_set_hidden(slot, !current || nt_view_child(slot, 0) != current);
	}

	if (!rc)
		rc = cli_file_browser_tab_labels(browser);

	if (!rc)
		rc = cli_file_browser_reveal(browser);

	return rc;
}

int cli_file_browser_focus(struct nt_view *view)
{
	struct cli_file_browser *browser = (struct cli_file_browser *)view;
	struct nt_view *current = cli_file_browser_current(browser);
	return browser->viewing && current ? cli_file_viewer_focus(current)
		: nt_view_focus(browser->list);
}

int cli_file_browser_list(struct nt_view *view)
{
	return cli_file_browser_show((struct cli_file_browser *)view, 0);
}

int cli_file_browser_focused(const struct nt_view *view)
{
	if (nt_view_is_focused(view))
		return 1;

	for (size_t i = 0; i < nt_view_child_count(view); i++) {
		if (cli_file_browser_focused(nt_view_child(view, i)))
			return 1;
	}

	return 0;
}

int cli_file_browser_open(struct nt_view *view, const char *path, int line)
{
	struct cli_file_browser *browser = (struct cli_file_browser *)view;
	struct nt_view *viewer = NULL;
	size_t index = 0;
	for (; index < browser->viewers.len; index++) {
		viewer = *(struct nt_view **)nb_vec_at(&browser->viewers, index);
		if (!strcmp(cli_file_viewer_path(viewer), path))
			break;
	}

	if (index == browser->viewers.len) {
		if (!nb_vec_reserve(&browser->viewers, index + 1))
			return NB_ERR_NOMEM;

		struct nt_view *slot = NULL;
		for (size_t i = 0; i < nt_view_child_count(browser->pages); i++) {
			struct nt_view *candidate = nt_view_child(browser->pages, i);
			if (!nt_view_child_count(candidate)) {
				slot = candidate;
				break;
			}
		}

		if (!slot)
			slot = nt_view_add(browser->builder, browser->pages, &nt_view_base_class,
				&(struct nt_view_options){.width = NT_FILL, .height = NT_FILL});

		viewer = slot ? cli_file_viewer_add(browser->builder, slot, path,
			browser->workdir, browser->styled) : NULL;
		int rc = nt_builder_error(browser->builder);
		if (rc)
			return rc;

		struct nt_view **owner = nb_vec_push(&browser->viewers);
		if (!owner)
			return NB_ERR_NOMEM;

		*owner = viewer;
	}

	browser->tabs_dirty = 1;
	browser->selected = index;
	browser->viewing = browser->reveal = 1;
	int rc = cli_file_viewer_open(viewer, line, 0);
	if (!rc)
		rc = cli_file_browser_sync(view);

	return rc;
}

void cli_file_browser_reset(struct nt_view *view)
{
	struct cli_file_browser *browser = (struct cli_file_browser *)view;
	(void)nt_view_clear_children(browser->pages);
	nb_vec_clear(&browser->viewers);
	nb_vec_clear(&browser->tab_widths);
	browser->selected = 0;
	browser->viewing = 0;
	browser->revision = UINT64_MAX;
	browser->tabs_dirty = 1;
	nt_view_invalidate(view, 1);
}
