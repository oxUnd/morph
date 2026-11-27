#include "sapi/cli/components/chat_dock.h"

#include "sapi/cli/components/composer.h"
#include <nb/err.h>

struct cli_chat_dock {
	struct nt_view base;
	struct cli_chat_dock_content content;
	int back_rows, activity_rows, input_rows, footer_rows;
};

static int cli_chat_dock_input_measure(struct nt_view *view, int width,
	struct nt_view_size *size)
{
	*size = (struct nt_view_size){0};
	for (size_t index = 0; index < nt_view_child_count(view); index++) {
		struct nt_view *child = nt_view_child(view, index);
		if (nt_view_is_hidden(child))
			continue;

		return nt_view_measure(child, width, size);
	}

	return 0;
}

static int cli_chat_dock_input_layout(struct nt_view *view)
{
	for (size_t index = 0; index < nt_view_child_count(view); index++) {
		struct nt_view *child = nt_view_child(view, index);
		if (nt_view_is_hidden(child))
			continue;

		int rc = nt_view_arrange(child, nt_view_content_rect(view));
		if (rc)
			return rc;
	}

	return 0;
}

static const struct nt_view_class cli_chat_dock_input_class = {
	.superclass = &nt_view_base_class,
	.instance_size = sizeof(struct nt_view),
	.kind = "cli_chat_dock_input",
	.measure = cli_chat_dock_input_measure,
	.layout = cli_chat_dock_input_layout,
};

struct nt_view *cli_chat_dock_input_add(struct nt_builder *builder, struct nt_view *parent)
{
	return nt_view_add(builder, parent, &cli_chat_dock_input_class, NULL);
}

static int cli_chat_dock_measure(struct nt_view *view, int width, struct nt_view_size *size)
{
	struct cli_chat_dock *dock = (struct cli_chat_dock *)view;
	*size = (struct nt_view_size){width,
		dock->back_rows + dock->activity_rows + dock->input_rows + dock->footer_rows};
	return 0;
}

static int cli_chat_dock_layout(struct nt_view *view)
{
	struct cli_chat_dock *dock = (struct cli_chat_dock *)view;
	struct nt_view_rect slot = nt_view_content_rect(view);
	struct nt_view *views[] = {dock->content.back, dock->content.activity,
		dock->content.input, dock->content.footer};
	const int rows[] = {dock->back_rows, dock->activity_rows,
		dock->input_rows, dock->footer_rows};
	for (size_t index = 0; index < sizeof(views) / sizeof(views[0]); index++) {
		int height = rows[index] < slot.height ? rows[index] : slot.height;
		struct nt_view_rect rect = {slot.x, slot.y, slot.width, height};
		int rc = views[index] == dock->content.back
			? nt_view_place(views[index], rect, NT_ANCHOR_CENTER, NT_VIEW_PLACE_FIT)
			: nt_view_arrange(views[index], rect);
		if (rc)
			return rc;

		slot.y += height;
		slot.height -= height;
	}

	return 0;
}

static const struct nt_view_class cli_chat_dock_class = {
	.superclass = &nt_view_base_class,
	.instance_size = sizeof(struct cli_chat_dock),
	.kind = "cli_chat_dock",
	.measure = cli_chat_dock_measure,
	.layout = cli_chat_dock_layout,
};

struct nt_view *cli_chat_dock_add(struct nt_builder *builder, struct nt_view *parent)
{
	return nt_view_add(builder, parent, &cli_chat_dock_class, NULL);
}

int cli_chat_dock_set_content(struct nt_view *view, const struct cli_chat_dock_content *content)
{
	if (!view || view->type != &cli_chat_dock_class || !content ||
	    !content->back || !content->activity || !content->input || !content->footer ||
	    !content->composer || !content->editor || !content->panel)
		return NB_ERR_INVAL;

	((struct cli_chat_dock *)view)->content = *content;
	return 0;
}

int cli_chat_dock_fit(struct nt_view *view, int width, int height, int separated)
{
	if (!view || view->type != &cli_chat_dock_class || width < 0 || height < 0)
		return NB_ERR_INVAL;

	struct cli_chat_dock *dock = (struct cli_chat_dock *)view;
	int previous = dock->back_rows + dock->activity_rows + dock->input_rows + dock->footer_rows;
	const struct cli_chat_dock_content *content = &dock->content;
	int modal = !nt_view_is_hidden(content->panel);
	struct nt_view_size input = {0}, footer = {0}, activity = {0};
	int rc = nt_view_measure(modal ? content->panel : content->editor, width, &input);
	if (!rc)
		rc = nt_view_measure(content->footer, width, &footer);

	if (!rc)
		rc = nt_view_measure(content->activity, width, &activity);

	if (rc)
		return rc;

	int back = !nt_view_is_hidden(content->back);
	int gap = activity.height && height >= input.height + footer.height + activity.height + back + 4;
	struct nt_view_insets padding = nt_view_options(view).padding;
	if (padding.top != gap) {
		padding.top = gap;
		rc = nt_view_set_padding(view, padding);
		if (rc)
			return rc;
	}

	height -= gap;
	int remaining = height > 0 ? height - 1 : 0;
	dock->footer_rows = footer.height < remaining ? footer.height : remaining;
	remaining = height - dock->footer_rows;
	int header = modal && activity.height > 0 && remaining > 1 ? 1 : 0;
	dock->input_rows = input.height < remaining - header ? input.height : remaining - header;
	remaining -= dock->input_rows;
	dock->back_rows = !nt_view_is_hidden(content->back) && remaining > 0 ? 1 : 0;
	remaining -= dock->back_rows;
	dock->activity_rows = activity.height < remaining ? activity.height : remaining;
	int input_space = height - dock->footer_rows - dock->back_rows - dock->activity_rows;
	if (!modal) {
		rc = cli_composer_fit(content->composer, width, input_space, separated);
		if (!rc)
			rc = nt_view_measure(content->composer, width, &input);
	}

	if (!rc)
		dock->input_rows = input.height < input_space ? input.height : input_space;

	int current = dock->back_rows + dock->activity_rows + dock->input_rows + dock->footer_rows;
	if (!rc && current != previous)
		nt_view_invalidate(view, 1);

	return rc;
}
