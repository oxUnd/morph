#include "sapi/cli/components/file_viewer.h"

#include "sapi/cli/components/media.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/models/files.h"
#include "sapi/cli/services/file_preview.h"
#include "terminal/text.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/file.h>
#include <nothing/tui/input.h>
#include <nothing/tui/keymap.h>
#include <nothing/tui/shortcut.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/scroll_view.h>
#include <nothing/tui/views/stack_view.h>
#include <nothing/tui/views/text_view.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct cli_file_viewer {
	struct nt_view base;
	struct nt_builder *builder;
	struct cli_file_preview *preview;
	struct cli_file_preview_job *job;
	char *path;
	const char *workdir;
	struct nt_view *metadata, *document, *media_scroll, *media;
	nb_vec links;
	int styled, source, rendered_width, line;
};

static int cli_file_viewer_link(void *data, const struct nt_view_event *event)
{
	struct cli_file_viewer *viewer = data;
	if (event->target != viewer->document || !viewer->preview)
		return 0;

	size_t start = 0, end = 0;
	if (nt_text_view_selection_range(viewer->document, &start, &end) > 0 && start != end)
		return 0;

	size_t cursor = nt_text_view_cursor(viewer->document);
	for (size_t i = 0; i < viewer->links.len; i++) {
		const struct cli_messages_link *link = nb_vec_at(&viewer->links, i);
		if (cursor < link->start || cursor >= link->end)
			continue;

		nb_buf directory NB_CLEANUP(nb_buf_cleanup) = {0};
		nb_buf path NB_CLEANUP(nb_buf_cleanup) = {0};
		int line = 0;
		int rc = nb_path_dirname(&directory, viewer->preview->path);
		if (!rc)
			rc = cli_files_resolve(nb_buf_cstr(&directory), link->url, &path, &line);

		if (rc)
			return 0;

		if (line && !nb_buf_printf(&path, ":%d", line))
			return nb_buf_status(&path);

		return nt_view_emit(&viewer->base, NT_VIEW_EVENT_OPEN, SIZE_MAX,
			nb_buf_cstr(&path));
	}

	return 0;
}

static int cli_file_viewer_render(struct cli_file_viewer *viewer, int width)
{
	const struct cli_file_preview *preview = viewer->preview;
	if (!preview || viewer->job || preview->image || viewer->rendered_width == width)
		return NB_OK;

	int rc = nt_text_view_set_wrap_mode(viewer->document,
		preview->status || (preview->markdown && !viewer->source)
			? NT_TEXT_WRAP_WORD : NT_TEXT_WRAP_NONE);
	if (rc)
		return rc;

	if (preview->status) {
		cli_messages_links_cleanup(&viewer->links);
		rc = nt_text_view_set_text(viewer->document, "");
	} else if (preview->markdown && !viewer->source) {
		nb_buf rendered NB_CLEANUP(nb_buf_cleanup) = {0};
		rc = cli_markdown_render_document(&rendered,
			nb_buf_cstr(&preview->content), width, 0);
		if (!rc)
			rc = cli_messages_set_rendered_links(viewer->document,
				nb_buf_cstr(&rendered), viewer->styled, &viewer->links);

		if (!rc)
			rc = cli_messages_markdown_links(&viewer->links,
				nt_text_view_text(viewer->document),
				nb_buf_cstr(&preview->content));
	} else if (viewer->rendered_width < 0) {
		cli_messages_links_cleanup(&viewer->links);
		if (preview->highlighted) {
			rc = cli_messages_set_source_view(viewer->document,
				preview->highlighted, viewer->styled);
		} else {
			char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
				nb_buf_cstr(&preview->content), preview->content.len,
				MORPH_TERMINAL_TEXT_MULTILINE, NULL);
			rc = safe ? nt_text_view_set_text(viewer->document, safe) : NB_ERR_NOMEM;
		}
	}

	if (!rc)
		viewer->rendered_width = width;

	return rc;
}

static int cli_file_viewer_layout(struct nt_view *view)
{
	struct cli_file_viewer *viewer = (struct cli_file_viewer *)view;
	struct nt_view_rect content = nt_view_content_rect(view);
	int rc = cli_file_viewer_render(viewer, content.width);
	if (rc)
		return rc;

	int metadata_rows = content.height > 2 ? 2 : content.height;
	rc = nt_view_arrange(viewer->metadata, (struct nt_view_rect){
		content.x, content.y, content.width, metadata_rows});
	if (rc)
		return rc;

	struct nt_view *body = viewer->preview && viewer->preview->image
		? viewer->media_scroll : viewer->document;
	rc = nt_view_arrange(body, (struct nt_view_rect){content.x, content.y + metadata_rows,
		content.width, content.height - metadata_rows});
	if (rc)
		return rc;

	if (viewer->line > 0 && viewer->preview && !viewer->job && !viewer->preview->image) {
		const char *text = nt_text_view_text(viewer->document);
		const char *at = text;
		for (int i = 1; i < viewer->line && *at; i++) {
			const char *next = strchr(at, '\n');
			at = next ? next + 1 : at + strlen(at);
		}

		struct nt_text_layout layout NB_CLEANUP(nt_text_layout_deinit) = {0};
		nt_text_layout_init(&layout);
		rc = nt_text_layout_build(&layout, text,
			(size_t)(content.width > 0 ? content.width : 1),
			viewer->preview->markdown && !viewer->source
				? NT_TEXT_WRAP_WORD : NT_TEXT_WRAP_NONE);
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
		rc = nt_view_class_methods(viewer->document)->handle_event(
			viewer->document, &scroll);
		if (rc < 0)
			return rc;

		scroll.mouse.delta_y = row > INT_MAX ? INT_MAX : (int)row;
		rc = nt_view_class_methods(viewer->document)->handle_event(
			viewer->document, &scroll);
		if (rc < 0)
			return rc;

		rc = NB_OK;
		viewer->line = 0;
	}

	return rc;
}

static int cli_file_viewer_metadata(struct cli_file_viewer *viewer)
{
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *relative = viewer->path;
	size_t length = strlen(viewer->workdir);
	if (!strncmp(relative, viewer->workdir, length) && relative[length] == '/')
		relative += length + 1;

	const char *slash = strrchr(relative, '/');
	if (slash) {
		if (!nb_buf_append(&text, relative, (size_t)(slash - relative + 1)))
			return nb_buf_status(&text);
	} else if (!nb_buf_append_cstr(&text, "./"))
		return nb_buf_status(&text);

	const struct cli_file_preview *preview = viewer->preview;
	if (viewer->job) {
		nb_buf_printf(&text, " · %s Loading preview", CLI_FEED_RUNNING);
	} else if (preview && !preview->status && !preview->image) {
		size_t lines = preview->content.len &&
			nb_buf_cstr(&preview->content)[preview->content.len - 1] != '\n' ? 1 : 0;
		for (size_t i = 0; i < preview->content.len; i++) {
			if (nb_buf_cstr(&preview->content)[i] == '\n')
				lines++;
		}

		nb_buf_printf(&text, " · %s · %zu %s · %.1f KiB",
			preview->markdown && !viewer->source ? "Markdown" : "Source", lines,
			lines == 1 ? "line" : "lines",
			(double)preview->content.len / 1024.0);
	} else if (preview && preview->image)
		nb_buf_append_cstr(&text, " · Image");

	return nb_buf_ok(&text)
		? nt_label_set_text(viewer->metadata, nb_buf_cstr(&text)) : nb_buf_status(&text);
}

static void cli_file_viewer_prepare_deinit(struct nt_view *view)
{
	cli_file_preview_stop(&((struct cli_file_viewer *)view)->job);
}

static void cli_file_viewer_destroy(struct nt_view *view)
{
	struct cli_file_viewer *viewer = (struct cli_file_viewer *)view;
	cli_file_preview_free(viewer->preview);
	cli_messages_links_cleanup(&viewer->links);
	free(viewer->path);
}

static int cli_file_viewer_key(struct nt_view *view, const struct nt_event *event)
{
	struct cli_file_viewer *viewer = (struct cli_file_viewer *)view;
	unsigned mods = event->modifiers &
		~(unsigned)(NT_MOD_SHIFT | NT_MOD_CAPS_LOCK | NT_MOD_NUM_LOCK);
	if (event->pasting || event->action == NT_KEY_ACTION_RELEASE || mods)
		return 0;

	int key = event->key;
	if (event->type == NT_EVENT_TEXT && event->text && event->text_length == 1)
		key = (unsigned char)event->text[0];
	else if (event->type != NT_EVENT_KEY || (event->flags & NT_EVENT_TEXT_FOLLOWS))
		return 0;

	if (key == 'r' || key == 'R') {
		int rc = cli_file_viewer_open(view, 0, 1);
		return rc ? rc : 1;
	}

	if ((key == 'm' || key == 'M') && viewer->preview &&
	    viewer->preview->markdown && !viewer->preview->status) {
		viewer->source = !viewer->source;
		viewer->rendered_width = -1;
		nt_view_invalidate(view, 1);
		int rc = cli_file_viewer_metadata(viewer);
		return rc ? rc : 1;
	}

	return 0;
}

static int cli_file_viewer_capture(struct nt_view *view, const struct nt_event *event)
{
	struct cli_file_viewer *viewer = (struct cli_file_viewer *)view;
	if (event->type != NT_EVENT_KEY || event->pasting ||
	    event->action == NT_KEY_ACTION_RELEASE || !nt_view_is_focused(viewer->document))
		return 0;

	unsigned mods = event->modifiers & ~(unsigned)(NT_MOD_CAPS_LOCK | NT_MOD_NUM_LOCK);
	if (event->key != 6 && !((event->key == 'f' || event->key == 'F') && mods == NT_MOD_CTRL))
		return 0;

	/* Keep the advertised Ctrl+F alias on hosts whose native find chord is Cmd+F. */
	struct nt_event find = *event;
	find.key = 'f';
	find.modifiers = nt_shortcut_primary_modifier();
	return nt_view_class_methods(viewer->document)->handle_event(viewer->document, &find);
}

static const struct nt_keymap *cli_file_viewer_keymap(const struct nt_view *view)
{
	const struct cli_file_viewer *viewer = (const struct cli_file_viewer *)view;
	static const struct nt_key_binding bindings[] = {
		{.key = 'r', .help = "R", .description = "Reload file preview"},
		{.key = 'f', .modifiers = NT_MOD_CTRL, .description = "Find in file"},
		{.key = 'm', .help = "M", .description = "Toggle Markdown source"},
	};
	static const struct nt_keymap plain = {bindings, 2};
	static const struct nt_keymap markdown = {bindings, NT_ARRAY_SIZE(bindings)};
	return viewer->preview && viewer->preview->markdown && !viewer->preview->status
		? &markdown : &plain;
}

struct nt_view *cli_file_viewer_add(struct nt_builder *builder, struct nt_view *parent,
	const char *path, const char *workdir, int styled)
{
	static const struct nt_view_class type = {
		.superclass = &nt_view_base_class,
		.instance_size = sizeof(struct cli_file_viewer),
		.kind = "cli_file_viewer",
		.layout = cli_file_viewer_layout,
		.capture_event = cli_file_viewer_capture,
		.handle_event = cli_file_viewer_key,
		.keymap = cli_file_viewer_keymap,
		.prepare_deinit = cli_file_viewer_prepare_deinit,
		.deinit = cli_file_viewer_destroy,
	};
	char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(path);
	if (!copy) {
		nt_builder_fail(builder, NB_ERR_NOMEM);
		return NULL;
	}

	struct cli_file_viewer *viewer = (struct cli_file_viewer *)nt_view_add(builder,
		parent, &type, &(struct nt_view_options){.width = NT_FILL, .height = NT_FILL});
	if (!viewer)
		return NULL;

	viewer->builder = builder;
	nt_builder_fail(builder, nt_view_set_help_label(&viewer->base, "File preview"));
	viewer->path = nb_charp_steal(&copy);
	viewer->workdir = workdir;
	viewer->styled = styled;
	viewer->rendered_width = -1;
	viewer->metadata = nt_label_add(builder, &viewer->base, "",
		&(struct nt_view_options){.style.attributes = styled ? NT_STYLE_DIM : 0});
	viewer->document = nt_text_view_add(builder, &viewer->base, "", NULL);
	viewer->media_scroll = nt_scroll_view_add(builder, &viewer->base, NULL);
	viewer->media = nt_column_add(builder, viewer->media_scroll, 0,
		&(struct nt_view_options){.width = NT_FILL});
	if (!nt_builder_error(builder)) {
		nt_builder_fail(builder, nt_label_set_wrap_mode(viewer->metadata, NT_TEXT_WRAP_NONE));
		nt_builder_fail(builder, nt_view_set_selectable(viewer->metadata, 0));
		nt_builder_fail(builder, nt_view_set_hidden(viewer->media_scroll, 1));
		if (!nt_view_on(viewer->document, NT_VIEW_EVENT_CLICK,
			cli_file_viewer_link, viewer))
			nt_builder_fail(builder, NB_ERR_NOMEM);
	}

	return &viewer->base;
}

const char *cli_file_viewer_path(const struct nt_view *view)
{
	return ((const struct cli_file_viewer *)view)->path;
}

const char *cli_file_viewer_hint(const struct nt_view *view)
{
	const struct cli_file_viewer *viewer = (const struct cli_file_viewer *)view;
	if (viewer->preview && viewer->preview->image)
		return "R reload · Ctrl+W close\nEsc files · Ctrl+O chat";

	if (viewer->preview && viewer->preview->markdown && !viewer->preview->status)
		return viewer->source
			? "Ctrl+F search · R reload · M preview\nCtrl+W close · Esc files · Ctrl+O chat"
			: "Ctrl+F search · R reload · M source\nCtrl+W close · Esc files · Ctrl+O chat";

	return "Ctrl+F search · R reload\nCtrl+W close · Esc files · Ctrl+O chat";
}

const char *cli_file_viewer_notice(const struct nt_view *view)
{
	const struct cli_file_viewer *viewer = (const struct cli_file_viewer *)view;
	const struct cli_file_preview *preview = viewer->preview;
	if (!preview || viewer->job)
		return "";

	if (preview->status) {
		return preview->status == NB_ERR_NOT_FOUND ? "The file no longer exists."
			: preview->status == NB_ERR_FORMAT
				? "Binary or non-UTF-8 file. Text preview is unavailable."
			: preview->status == NB_ERR_UNSUPPORTED
				? "Only regular files and supported images can be previewed."
			: preview->status == NB_ERR_TOO_BIG
				? "This file exceeds the 32 MiB preview limit."
				: nb_status_str(preview->status);
	}

	return preview->truncated ? "File preview limited to 1 MiB." : "";
}

int cli_file_viewer_open(struct nt_view *view, int line, int reload)
{
	struct cli_file_viewer *viewer = (struct cli_file_viewer *)view;
	if (line > 0) {
		viewer->line = line;
		if (!viewer->source) {
			viewer->source = 1;
			viewer->rendered_width = -1;
		}
	}

	if (reload)
		cli_file_preview_stop(&viewer->job);

	int rc = NB_OK;
	if (!viewer->job && (!viewer->preview || reload))
		rc = cli_file_preview_start(viewer->path, &viewer->job);

	if (!rc && !viewer->preview)
		rc = nt_text_view_set_text(viewer->document, "Loading file preview...");

	if (!rc)
		rc = cli_file_viewer_metadata(viewer);

	nt_view_invalidate(view, 1);
	return rc;
}

int cli_file_viewer_sync(struct nt_view *view)
{
	struct cli_file_viewer *viewer = (struct cli_file_viewer *)view;
	if (!viewer->job)
		return NB_OK;

	struct cli_file_preview *preview NB_CLEANUP(cli_file_preview_cleanup) = NULL;
	int rc = cli_file_preview_collect(&viewer->job, &preview);
	if (rc == NB_ERR_AGAIN)
		return NB_OK;

	if (rc || !preview)
		return rc;

	int reading = nt_view_is_focused(viewer->document) ||
		nt_view_is_focused(viewer->media_scroll);
	cli_file_preview_free(viewer->preview);
	viewer->preview = preview;
	preview = NULL;
	viewer->rendered_width = -1;
	cli_messages_links_cleanup(&viewer->links);
	rc = nt_view_set_hidden(viewer->document, viewer->preview->image);
	if (!rc)
		rc = nt_view_set_hidden(viewer->media_scroll, !viewer->preview->image);

	if (!rc)
		rc = nt_view_clear_children(viewer->media);

	if (!rc && viewer->preview->image)
		cli_media_add(viewer->builder, viewer->media, viewer->styled,
			"image", viewer->path, NULL);

	if (!rc)
		rc = nt_builder_error(viewer->builder);

	if (!rc)
		rc = cli_file_viewer_metadata(viewer);

	if (!rc && reading)
		rc = cli_file_viewer_focus(view);

	nt_view_invalidate(view, 1);
	return rc;
}

int cli_file_viewer_focus(struct nt_view *view)
{
	struct cli_file_viewer *viewer = (struct cli_file_viewer *)view;
	return nt_view_focus(viewer->preview && viewer->preview->image
		? viewer->media_scroll : viewer->document);
}
