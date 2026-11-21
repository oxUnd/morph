#include "sapi/cli/pages/rewind.h"
#include "sapi/cli/services/checkpoints.h"
#include "sapi/cli/components/page_layout.h"
#include "sapi/cli/presentation/panel.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/presentation/list.h"
#include <nb/cleanup.h>
#include "cJSON.h"
#include "errors.h"
#include <nothing/tui/models/list_navigation.h>
#include <nothing/tui/controls/list_view.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/text_view.h>
#include <nothing/tui/terminal/text.h>
#include <nothing/tui/screen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cli_rewind_picker {
	struct nt_view_controller base;
	struct cli_page_layout layout;
	size_t selected;
	cJSON *listing;
	cJSON *preview;
	struct nt_view *heading[4], *list, *files, *footer;
	cli_rewind_apply_fn apply;
	int64_t id;
	int stage;
	enum runtime_rewind_mode mode;
	char error[256];
};

static const char *cli_rewind_prompt(const cJSON *entry)
{
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "prompt"));
	return text ? text : "";
}

static int cli_rewind_available(const struct cli_rewind_picker *picker,
				enum runtime_rewind_mode mode)
{
	if ((mode & RUNTIME_REWIND_CODE) &&
	    (cJSON_IsFalse(cJSON_GetObjectItem(picker->preview, "code_active")) ||
	     cJSON_IsFalse(cJSON_GetObjectItem(picker->preview, "code_restorable"))))
		return 0;

	return !(mode & RUNTIME_REWIND_CONVERSATION) ||
	       !cJSON_IsFalse(cJSON_GetObjectItem(picker->preview, "conversation_restorable"));
}

static const char *cli_rewind_reason(const struct cli_rewind_picker *picker)
{
	const char *reason =
		cJSON_GetStringValue(cJSON_GetObjectItem(picker->preview, "invalid_reason"));
	if (!reason || !*reason)
		reason = cJSON_GetStringValue(cJSON_GetObjectItem(picker->preview, "reason"));

	return reason && *reason ? reason
				 : "Code restore is unavailable; inspect conflicts or choose "
				   "conversation only.";
}

static const char *const cli_rewind_names[] = {"Code and conversation", "Code only",
					       "Conversation only"};
static const enum runtime_rewind_mode cli_rewind_modes[] = {
	RUNTIME_REWIND_BOTH, RUNTIME_REWIND_CODE, RUNTIME_REWIND_CONVERSATION};

static void cli_rewind_clip(char *output, size_t capacity, const char *text, int width)
{
	char compact[BUFSIZ];
	cli_list_compact_text(compact, sizeof(compact), text);
	nt_terminal_copy_ellipsized_display_width(output, capacity, compact,
						  width > 0 ? (size_t)width : 0, 0);
}

static int cli_rewind_row(void *data, struct nt_canvas *canvas, size_t item, size_t group,
			  int selected)
{
	struct cli_rewind_picker *page = data;
	(void)group;
	char text[BUFSIZ], clipped[BUFSIZ];
	const struct nt_text_style style =
		selected ? (struct nt_text_style){.attributes = NT_STYLE_BOLD,
						  .foreground = {NT_COLOR_ANSI, 6}}
			 : (struct nt_text_style){0};
	if (page->stage) {
		snprintf(
			text, sizeof(text), "%s %s%s", selected ? ">" : " ", cli_rewind_names[item],
			cli_rewind_available(page, cli_rewind_modes[item]) ? "" : " (unavailable)");
	} else {
		const cJSON *entry = cJSON_GetArrayItem(
			cJSON_GetObjectItem(page->listing, "checkpoints"), (int)item);
		snprintf(text, sizeof(text), "%s #%lld  %s", selected ? ">" : " ",
			 (long long)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "id")),
			 cli_rewind_prompt(entry));
	}

	cli_rewind_clip(clipped, sizeof(clipped), text, canvas->width);
	int rc = nt_canvas_fill_styled(canvas, &style);
	return rc ? rc : nt_canvas_text_styled(canvas, 0, 0, clipped, &style);
}

static int cli_rewind_layout(struct cli_rewind_picker *page)
{
	struct nt_view_rect rect = nt_view_content_rect(page->layout.body);
	int h = rect.height, w = rect.width;
	int rc = cli_page_layout_set_subpage(&page->layout, page->stage ? "restore" : "");
	if (rc || h <= 0 || w <= 0)
		return rc;

	int first = page->stage && h >= 10 ? 4 : page->stage && h >= 8 ? 3 : h >= 6 ? 2 : 1;
	const cJSON *files = cJSON_GetObjectItem(page->preview, "files");
	char counts[96];
	snprintf(counts, sizeof(counts), "#%lld · %d files · %d conflicts", (long long)page->id,
		 cJSON_GetArraySize(files),
		 cJSON_GetArraySize(cJSON_GetObjectItem(page->preview, "conflicts")));
	const char *reason =
		!cli_rewind_available(page, RUNTIME_REWIND_CODE) ? cli_rewind_reason(page) : NULL;
	const char *heading[] = {
		page->stage && h < 8 ? cli_rewind_prompt(page->preview)
		: page->stage	     ? "Rewind · choose what to restore"
				     : "Rewind · choose a turn",
		page->stage && h >= 8 ? cli_rewind_prompt(page->preview)
		: page->stage	      ? (reason ? reason : counts)
		: *page->error	      ? page->error
				      : "Each checkpoint precedes its user turn.",
		h < 10 && reason ? reason : counts,
		*page->error ? page->error
		: reason     ? reason
			     : "Files below will return to their state before this turn."};
	for (int i = 0; i < 4 && !rc; i++) {
		rc = nt_view_set_hidden(page->heading[i], i >= first);
		if (!rc && i < first) {
			char text[BUFSIZ];
			cli_rewind_clip(text, sizeof(text), heading[i], w);
			rc = nt_label_set_text(page->heading[i], text);
			if (!rc)
				rc = nt_view_set_frame(page->heading[i],
						       (struct nt_view_rect){0, i, w, 1});
		}
	}

	int rows = h - first;
	if (rows < 1)
		rows = 1;

	int choices =
		page->stage ? 3
			    : cJSON_GetArraySize(cJSON_GetObjectItem(page->listing, "checkpoints"));
	if (choices < rows)
		rows = choices;

	if (!rc)
		rc = nt_view_set_frame(page->list, (struct nt_view_rect){0, first, w, rows});

	int space = h - first - rows;
	if (!rc)
		rc = nt_view_set_hidden(page->files, !page->stage || space <= 0);

	if (!rc && page->stage && space > 0) {
		nb_buf lines NB_CLEANUP(nb_buf_cleanup) = {0};
		for (int i = 0; i < space && i < cJSON_GetArraySize(files); i++) {
			const char *path = cJSON_GetStringValue(
				cJSON_GetObjectItem(cJSON_GetArrayItem(files, i), "path"));
			char compact[BUFSIZ];
			cli_rewind_clip(compact, sizeof(compact), path ? path : "", w);
			nb_buf_printf(&lines, "%s%s", i ? "\n" : "", compact);
		}

		rc = nb_buf_status(&lines);
		if (!rc)
			rc = nt_text_view_set_text(page->files, nb_buf_cstr(&lines));

		if (!rc)
			rc = nt_view_set_frame(page->files,
					       (struct nt_view_rect){0, first + rows, w, space});
	}

	if (!rc)
		rc = nt_view_set_hidden(page->footer, h < 3);

	if (!rc) {
		char text[BUFSIZ];
		const char *hint =
			page->stage ? "Up/Down Select · Enter Restore · Esc Back"
				    : "Up/Down Select · Enter Preview · Esc Cancel";
		cli_rewind_clip(text, sizeof(text), hint, w);
		rc = nt_label_set_text(page->footer, text);
	}

	return rc;
}

static int cli_rewind_choices(struct cli_rewind_picker *page, size_t selected)
{
	int choices =
		page->stage ? 3
			    : cJSON_GetArraySize(cJSON_GetObjectItem(page->listing, "checkpoints"));
	int rc = nt_list_view_set_source(page->list, (size_t)choices, NULL, NULL);
	if (!rc)
		rc = nt_list_view_select(page->list, selected);

	return rc ? rc : cli_rewind_layout(page);
}

static int cli_rewind_activate(void *data, const struct nt_view_event *event)
{
	struct cli_rewind_picker *page = data;
	size_t index = event->index;
	if (page->stage) {
		if (!cli_rewind_available(page, cli_rewind_modes[index])) {
			snprintf(page->error, sizeof(page->error),
				 "That restore mode is unavailable.");
			return cli_rewind_layout(page);
		}

		page->mode = cli_rewind_modes[index];
		return nt_view_controller_dismiss(&page->base, 0);
	}

	const cJSON *entry =
		cJSON_GetArrayItem(cJSON_GetObjectItem(page->listing, "checkpoints"), (int)index);
	page->selected = index;
	page->id = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "id"));
	cJSON_DeleteOwner(&page->preview);
	struct cli_application *ctx = cli_application_from_controller(&page->base);
	int rc = cli_checkpoints_preview(ctx, page->id, &page->preview);
	if (rc) {
		snprintf(page->error, sizeof(page->error), "Preview failed: %s",
			 nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return cli_rewind_layout(page);
	}

	page->stage = 1;
	page->error[0] = '\0';
	return cli_rewind_choices(page, cli_rewind_available(page, RUNTIME_REWIND_BOTH) ? 0
					: cli_rewind_available(page, RUNTIME_REWIND_CONVERSATION)
						? 2
						: 1);
}

static int cli_rewind_back(void *data)
{
	struct cli_rewind_picker *page = data;
	page->stage = 0;
	page->error[0] = '\0';
	int rc = cli_rewind_choices(page, page->selected);
	return rc ? rc : nt_view_focus(page->list);
}

static int cli_rewind_size(void *data, const struct nt_view_event *event)
{
	struct cli_rewind_picker *page = data;
	return event->target == page->layout.body ? cli_rewind_layout(page) : 0;
}

static void cli_rewind_build(struct nt_view_controller *controller, struct nt_builder *builder)
{
	struct cli_rewind_picker *page = (struct cli_rewind_picker *)controller;
	cli_page_layout_build(builder, &page->layout, "rewind", &nt_view_base_class,
			      cli_rewind_back, page);
	struct nt_view *root = page->layout.body;
	const struct nt_view_options heading = cli_panel_heading(NT_AUTO);
	const struct nt_view_options hint = cli_panel_hint(NT_AUTO);
	for (int i = 0; i < 4; i++)
		page->heading[i] = nt_label_add(builder, root, "", i == 0 ? &heading : NULL);

	page->list = nt_list_view_add(builder, root, NULL, 0, 0, NULL);
	page->files = nt_text_view_add(builder, root, "", NULL);
	page->footer = nt_label_add(builder, page->layout.footer, "", &hint);
	if (nt_builder_error(builder))
		return;

	nt_view_set_focusable(page->files, 0);
	int rc = nt_list_view_set_row_renderer(page->list, cli_rewind_row, page);
	if (!rc)
		rc = nt_list_view_set_navigation(page->list, NT_LIST_VIEW_VIM | NT_LIST_VIEW_PAGED);

	if (!rc && (!nt_view_on(page->list, NT_VIEW_EVENT_ACTIVATE, cli_rewind_activate, page) ||
		    !nt_view_on(root, NT_VIEW_EVENT_SIZE_CHANGED, cli_rewind_size, page)))
		rc = NB_ERR_NOMEM;

	nt_builder_fail(builder, rc ? rc : cli_rewind_choices(page, 0));
	nt_builder_fail(builder, nt_view_focus(page->list));
}

static int cli_rewind_event(struct nt_view_controller *controller, const struct nt_event *event)
{
	struct cli_rewind_picker *page = (struct cli_rewind_picker *)controller;
	if (event->type != NT_EVENT_KEY || event->pasting || event->action == NT_KEY_ACTION_RELEASE)
		return 0;

	if ((event->key == NT_KEY_LEFT || event->key == 27 || event->key == NT_KEY_ESCAPE) &&
	    page->stage)
		return cli_rewind_back(page);

	if (event->key == 'q' || event->key == 27 || event->key == NT_KEY_ESCAPE ||
	    event->key == 3 || event->key == 4 || event->key == NT_KEY_LEFT)
		return nt_view_controller_dismiss(controller, 0);

	return 0;
}

static void cli_rewind_deinit(struct nt_view_controller *controller)
{
	struct cli_rewind_picker *page = (struct cli_rewind_picker *)controller;
	struct cli_application *ctx = cli_application_from_controller(controller);
	int result = nt_view_controller_result(controller);
	if (ctx && !result && page->mode)
		(void)page->apply(ctx, page->id, page->mode);

	cJSON_Delete(page->listing);
	cJSON_Delete(page->preview);
}

int cli_rewind_create_take(cJSON **listing, cli_rewind_apply_fn apply,
			   struct nt_view_controller **out)
{
	if (!listing || !*listing || !apply ||
	    cJSON_GetArraySize(cJSON_GetObjectItem(*listing, "checkpoints")) <= 0)
		return NB_ERR_INVAL;

	static const struct nt_view_controller_class type = {
		.superclass = &cli_application_panel_class,
		.instance_size = sizeof(struct cli_rewind_picker),
		.load_view = cli_rewind_build,
		.handle_event = cli_rewind_event,
		.deinit = cli_rewind_deinit,
	};
	int rc = nt_view_controller_create(&type, out);
	if (rc)
		return rc;

	struct cli_rewind_picker *page = (struct cli_rewind_picker *)*out;
	page->listing = cJSON_StealOwner(listing);
	page->apply = apply;
	return 0;
}
