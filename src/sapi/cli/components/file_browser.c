#include "sapi/cli/components/file_browser.h"

#include "sapi/cli/components/media.h"
#include "sapi/cli/models/files.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/services/file_preview.h"
#include "terminal/text.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/file.h>
#include <nothing/tui/controls/list_view.h>
#include <nothing/tui/input.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/scroll_view.h>
#include <nothing/tui/views/stack_view.h>
#include <nothing/tui/views/text_view.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct cli_file_browser {
	struct nt_view base;
	struct nt_builder *builder;
	struct cli_files_model *model;
	struct cli_file_preview *preview;
	const char *workdir;
	struct nt_view *heading, *list, *path, *document, *media_scroll, *media, *footer;
	uint64_t revision;
	nb_vec links;
	int styled, source, rendered_width, line, syncing, loading;
};

static const char *cli_file_browser_row(void *data, size_t index)
{
	struct cli_file_browser *browser = data;
	const struct cli_files_entry *entry = nb_vec_at(&browser->model->entries, index);
	if (!entry)
		return "";

	size_t length = strlen(browser->workdir);
	return !strncmp(entry->path, browser->workdir, length) && entry->path[length] == '/'
		? entry->path + length + 1 : nb_path_basename(entry->path);
}

static int cli_file_browser_link(void *data, const struct nt_view_event *event)
{
	struct cli_file_browser *browser = data;
	if (event->target != browser->document || !browser->preview)
		return 0;

	size_t start = 0, end = 0;
	if (nt_text_view_selection_range(browser->document, &start, &end) > 0 && start != end)
		return 0;

	size_t cursor = nt_text_view_cursor(browser->document);
	for (size_t i = 0; i < browser->links.len; i++) {
		const struct cli_messages_link *link = nb_vec_at(&browser->links, i);
		if (cursor < link->start || cursor >= link->end)
			continue;

		nb_buf directory NB_CLEANUP(nb_buf_cleanup) = {0};
		nb_buf path NB_CLEANUP(nb_buf_cleanup) = {0};
		int line = 0;
		int rc = nb_path_dirname(&directory, browser->preview->path);
		if (!rc)
			rc = cli_files_resolve(nb_buf_cstr(&directory), link->url, &path, &line);

		if (rc)
			return 0;

		if (line && !nb_buf_printf(&path, ":%d", line))
			return nb_buf_status(&path);

		return nt_view_emit(&browser->base, NT_VIEW_EVENT_OPEN, SIZE_MAX,
			nb_buf_cstr(&path));
	}

	return 0;
}

static int cli_file_browser_render(struct cli_file_browser *browser, int width)
{
	const struct cli_file_preview *preview = browser->preview;
	if (!preview || browser->loading || preview->image || browser->rendered_width == width)
		return NB_OK;

	int rc = NB_OK;
	if (preview->status) {
		const char *message = preview->status == NB_ERR_NOT_FOUND
			? "The file no longer exists."
			: preview->status == NB_ERR_FORMAT
				? "Binary or non-UTF-8 file. Text preview is unavailable."
			: preview->status == NB_ERR_UNSUPPORTED
				? "Only regular files and supported images can be previewed."
			: preview->status == NB_ERR_TOO_BIG
				? "This file exceeds the 32 MiB preview limit."
				: nb_status_str(preview->status);
		nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
		int unavailable = preview->status == NB_ERR_FORMAT ||
			preview->status == NB_ERR_UNSUPPORTED;
		if (!nb_buf_printf(&text, "%s%s%s %s\n\nR retry · Esc close",
			browser->styled && !unavailable ? CLI_FEED_FAILED_STYLE : "",
			unavailable ? CLI_FEED_SYSTEM : CLI_FEED_FAILED,
			browser->styled ? ANSI_RESET : "", message))
			return nb_buf_status(&text);

		rc = cli_messages_set_rendered_links(browser->document, nb_buf_cstr(&text),
			browser->styled, &browser->links);
	} else if (preview->markdown && !browser->source) {
		nb_buf rendered NB_CLEANUP(nb_buf_cleanup) = {0};
		rc = cli_markdown_render_document(&rendered,
			nb_buf_cstr(&preview->content), width, 0);
		if (!rc)
			rc = cli_messages_set_rendered_links(browser->document,
				nb_buf_cstr(&rendered), browser->styled, &browser->links);

		if (!rc)
			rc = cli_messages_markdown_links(&browser->links,
				nt_text_view_text(browser->document),
				nb_buf_cstr(&preview->content));
	} else if (browser->rendered_width < 0) {
		cli_messages_links_cleanup(&browser->links);
		if (preview->highlighted) {
			rc = cli_messages_set_source_view(browser->document,
				preview->highlighted, browser->styled);
		} else {
			char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
				nb_buf_cstr(&preview->content), preview->content.len,
				MORPH_TERMINAL_TEXT_MULTILINE, NULL);
			rc = safe ? nt_text_view_set_text(browser->document, safe) : NB_ERR_NOMEM;
		}
	}

	if (!rc)
		browser->rendered_width = width;

	return rc;
}

static int cli_file_browser_layout(struct nt_view *view)
{
	struct cli_file_browser *browser = (struct cli_file_browser *)view;
	struct nt_view_rect content = nt_view_content_rect(view);
	int rc = cli_file_browser_render(browser, content.width);
	if (rc)
		return rc;

	int list_rows = (int)(browser->model->entries.len < 6 ? browser->model->entries.len : 6);
	if (list_rows > content.height / 3)
		list_rows = content.height / 3;

	int fixed[] = {1, list_rows, browser->preview ? 2 : 0, -1, 2};
	struct nt_view *body = browser->preview && browser->preview->image && !browser->loading
		? browser->media_scroll : browser->document;
	struct nt_view *children[] = {
		browser->heading, browser->list, browser->path, body, browser->footer,
	};
	int used = 0;
	for (size_t i = 0; i < sizeof(children) / sizeof(children[0]); i++) {
		int height = fixed[i] < 0 ? content.height - used - 2 : fixed[i];
		if (height < 0)
			height = 0;

		if (height > content.height - used)
			height = content.height - used;

		rc = nt_view_arrange(children[i], (struct nt_view_rect){
			content.x, content.y + used, content.width, height});
		if (rc)
			return rc;

		used += height;
	}

	if (browser->line > 0 && browser->preview && !browser->preview->image) {
		const char *text = nt_text_view_text(browser->document);
		const char *at = text;
		for (int i = 1; i < browser->line && *at; i++) {
			const char *next = strchr(at, '\n');
			at = next ? next + 1 : at + strlen(at);
		}

		struct nt_text_layout layout NB_CLEANUP(nt_text_layout_deinit) = {0};
		nt_text_layout_init(&layout);
		rc = nt_text_layout_build(&layout, text,
			(size_t)(content.width > 0 ? content.width : 1), NT_TEXT_WRAP_WORD);
		if (rc)
			return rc;

		size_t row = 0;
		for (size_t i = 0; i < layout.lines.len; i++) {
			const struct nt_text_line *line = nb_vec_at(&layout.lines, i);
			if (line->offset > (size_t)(at - text))
				break;

			row = i;
		}

		struct nt_event scroll = {.type = NT_EVENT_SCROLL};
		scroll.mouse.delta_y = -INT_MAX;
		rc = nt_view_class_methods(browser->document)->handle_event(
			browser->document, &scroll);
		if (rc < 0)
			return rc;

		scroll.mouse.delta_y = row > INT_MAX ? INT_MAX : (int)row;
		rc = nt_view_class_methods(browser->document)->handle_event(
			browser->document, &scroll);
		if (rc < 0)
			return rc;

		rc = NB_OK;
		browser->line = 0;
	}

	return rc;
}

static void cli_file_browser_destroy(struct nt_view *view)
{
	cli_file_preview_free(((struct cli_file_browser *)view)->preview);
	cli_messages_links_cleanup(&((struct cli_file_browser *)view)->links);
}

static int cli_file_browser_activate(void *data, const struct nt_view_event *event)
{
	struct cli_file_browser *browser = data;
	if (browser->syncing || event->target != browser->list)
		return 0;

	size_t index = nt_list_view_selected(browser->list);
	const struct cli_files_entry *entry = nb_vec_at(&browser->model->entries, index);
	return entry ? nt_view_emit(&browser->base, NT_VIEW_EVENT_OPEN, index, entry->path) : 0;
}

static int cli_file_browser_key(struct nt_view *view, const struct nt_event *event)
{
	struct cli_file_browser *browser = (struct cli_file_browser *)view;
	if (event->pasting || event->type != NT_EVENT_KEY ||
	    event->action == NT_KEY_ACTION_RELEASE)
		return 0;

	int key = event->key;
	if (key == 27 || key == NT_KEY_ESCAPE || key == 3)
		return nt_view_emit(view, NT_VIEW_EVENT_CANCEL, 0, NULL);

	if (browser->loading)
		return 0;

	if ((key == 'r' || key == 'R') && browser->preview)
		return nt_view_emit(view, NT_VIEW_EVENT_ACTIVATE, 0, browser->preview->path);

	if ((key == 'm' || key == 'M') && browser->preview && browser->preview->markdown) {
		browser->source = !browser->source;
		browser->rendered_width = -1;
		browser->revision = UINT64_MAX;
		nt_view_invalidate(view, 1);
		return 1;
	}

	return 0;
}

struct nt_view *cli_file_browser_add(struct nt_builder *builder, struct nt_view *parent,
	struct cli_files_model *model, const char *workdir, int styled)
{
	static const struct nt_view_class type = {
		.superclass = &nt_view_base_class,
		.instance_size = sizeof(struct cli_file_browser),
		.kind = "cli_file_browser",
		.layout = cli_file_browser_layout,
		.handle_event = cli_file_browser_key,
		.deinit = cli_file_browser_destroy,
	};
	const struct nt_view_options options = {
		.width = NT_FILL, .height = NT_FILL, .padding = {.left = 1, .right = 1},
	};
	struct cli_file_browser *browser = (struct cli_file_browser *)
		nt_view_add(builder, parent, &type, &options);
	if (!browser)
		return NULL;

	browser->builder = builder;
	browser->model = model;
	browser->workdir = workdir;
	browser->styled = styled;
	browser->revision = UINT64_MAX;
	browser->rendered_width = -1;
	browser->heading = nt_label_add(builder, &browser->base, "Files", NULL);
	browser->list = nt_list_view_add(builder, &browser->base, NULL, 0, 0, NULL);
	browser->path = nt_label_add(builder, &browser->base, "", NULL);
	browser->document = nt_text_view_add(builder, &browser->base,
		"Files referenced, modified or generated in this conversation appear here.", NULL);
	browser->media_scroll = nt_scroll_view_add(builder, &browser->base, NULL);
	browser->media = nt_column_add(builder, browser->media_scroll, 0,
		&(struct nt_view_options){.width = NT_FILL});
	browser->footer = nt_label_add(builder, &browser->base,
		"Enter open · Ctrl+O chat · Ctrl+F search\nR reload · M source · Esc close", NULL);
	if (!nt_builder_error(builder)) {
		nt_builder_fail(builder, nt_view_set_hidden(browser->media_scroll, 1));
		nt_builder_fail(builder, nt_list_view_set_activate_on_click(browser->list, 1));
		if (!nt_view_on(browser->list, NT_VIEW_EVENT_ACTIVATE,
			cli_file_browser_activate, browser) ||
		    !nt_view_on(browser->document, NT_VIEW_EVENT_CLICK,
			cli_file_browser_link, browser))
			nt_builder_fail(builder, NB_ERR_NOMEM);
	}

	return &browser->base;
}

int cli_file_browser_sync(struct nt_view *view)
{
	struct cli_file_browser *browser = (struct cli_file_browser *)view;
	if (browser->revision == browser->model->revision)
		return NB_OK;

	nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!nb_buf_printf(&title, "Files (%zu)%s", browser->model->entries.len,
		browser->model->truncated ? " · limit reached" :
		browser->preview && browser->preview->truncated ? " · preview limited to 1 MiB" :
		browser->preview && browser->preview->markdown && browser->source
			? " · Source" : ""))
		return nb_buf_status(&title);

	browser->syncing = 1;
	int rc = nt_list_view_set_source(browser->list, browser->model->entries.len,
		cli_file_browser_row, browser);
	if (!rc)
		rc = nt_label_set_text(browser->heading, nb_buf_cstr(&title));

	browser->syncing = 0;
	if (!rc)
		browser->revision = browser->model->revision;

	return rc;
}

int cli_file_browser_focus(struct nt_view *view)
{
	struct cli_file_browser *browser = (struct cli_file_browser *)view;
	return nt_view_focus(browser->model->entries.len ? browser->list : browser->document);
}

int cli_file_browser_focus_content(struct nt_view *view)
{
	struct cli_file_browser *browser = (struct cli_file_browser *)view;
	return nt_view_focus(browser->preview && browser->preview->image
		&& !browser->loading ? browser->media_scroll : browser->document);
}

int cli_file_browser_focused(const struct nt_view *view)
{
	const struct cli_file_browser *browser = (const struct cli_file_browser *)view;
	return nt_view_is_focused(browser->list) || nt_view_is_focused(browser->document) ||
		nt_view_is_focused(browser->media_scroll);
}

int cli_file_browser_loading(struct nt_view *view, const char *path)
{
	struct cli_file_browser *browser = (struct cli_file_browser *)view;
	browser->loading = 1;
	cli_messages_links_cleanup(&browser->links);
	nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!nb_buf_printf(&title, "%s Loading preview", CLI_FEED_RUNNING))
		return nb_buf_status(&title);

	int rc = nt_label_set_text(browser->heading, nb_buf_cstr(&title));
	if (!rc)
		rc = nt_text_view_set_text(browser->document, "Loading file preview...");

	if (!rc)
		rc = nt_view_set_hidden(browser->document, 0);

	if (!rc)
		rc = nt_view_set_hidden(browser->media_scroll, 1);

	if (!rc) {
		size_t length = strlen(browser->workdir);
		const char *label = !strncmp(path, browser->workdir, length) && path[length] == '/'
			? path + length + 1 : path;
		rc = nt_label_set_text(browser->path, label);
	}

	return rc;
}

int cli_file_browser_preview_take(struct nt_view *view, struct cli_file_preview **owner, int line)
{
	struct cli_file_browser *browser = (struct cli_file_browser *)view;
	if (!owner || !*owner)
		return NB_ERR_INVAL;

	char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		(*owner)->path, strlen((*owner)->path), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;

	size_t length = strlen(browser->workdir);
	const char *label = !strncmp(safe, browser->workdir, length) && safe[length] == '/'
		? safe + length + 1 : safe;
	int rc = nt_label_set_text(browser->path, label);
	if (rc)
		return rc;

	cli_file_preview_free(browser->preview);
	browser->preview = *owner;
	*owner = NULL;
	browser->loading = 0;
	browser->source = line > 0;
	browser->line = line;
	browser->rendered_width = -1;
	browser->revision = UINT64_MAX;
	cli_messages_links_cleanup(&browser->links);
	int reading = nt_view_is_focused(browser->document) ||
		nt_view_is_focused(browser->media_scroll);
	rc = nt_view_set_hidden(browser->document, browser->preview->image);
	if (!rc)
		rc = nt_view_set_hidden(browser->media_scroll, !browser->preview->image);

	if (!rc)
		rc = nt_view_clear_children(browser->media);

	if (!rc && browser->preview->image)
		cli_media_add(browser->builder, browser->media, browser->styled,
			"image", browser->preview->path, NULL);

	if (!rc)
		rc = nt_builder_error(browser->builder);

	if (!rc && reading)
		rc = cli_file_browser_focus_content(view);

	if (!rc)
		rc = cli_file_browser_sync(view);

	nt_view_invalidate(view, 1);
	return rc;
}

void cli_file_browser_reset(struct nt_view *view)
{
	struct cli_file_browser *browser = (struct cli_file_browser *)view;
	cli_file_preview_free(browser->preview);
	cli_messages_links_cleanup(&browser->links);
	browser->preview = NULL;
	browser->loading = 0;
	browser->revision = UINT64_MAX;
	browser->rendered_width = -1;
	(void)nt_label_set_text(browser->path, "");
	(void)nt_text_view_set_text(browser->document,
		"Files referenced, modified or generated in this conversation appear here.");
	(void)nt_view_set_hidden(browser->document, 0);
	(void)nt_view_set_hidden(browser->media_scroll, 1);
	(void)nt_view_clear_children(browser->media);
}
