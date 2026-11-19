#include "sapi/cli/components/page_layout.h"

#include "sapi/cli/presentation/panel.h"

#include <nb/err.h>
#include <nothing/tui/view_controller.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/stack_view.h>

struct cli_page_layout_navigation {
	struct nt_view base;
	struct nt_view *home, *separator, *page, *branch, *subpage;
	cli_page_layout_back_fn back;
	void *data;
};

static int cli_page_layout_activate(void *data, const struct nt_view_event *event)
{
	struct cli_page_layout_navigation *nav = data;
	if (event->type == NT_VIEW_EVENT_KEY &&
	    (event->input->pasting || event->input->action == NT_KEY_ACTION_RELEASE ||
	     (event->input->key != '\r' && event->input->key != '\n' &&
	      event->input->key != ' ' && event->input->key != 27)))
		return 0;

	int rc = 0;
	if (event->type == NT_VIEW_EVENT_KEY && event->input->key == 27 &&
	    *nt_label_text(nav->subpage) && nav->back)
		rc = nav->back(nav->data);
	else if (event->target == nav->home)
		rc = nt_view_controller_dismiss(nt_view_controller(&nav->base), 0);
	else if (*nt_label_text(nav->subpage) && nav->back)
		rc = nav->back(nav->data);

	return rc < 0 ? rc : NT_VIEW_EVENT_STOP;
}

static int cli_page_layout_navigation_layout(struct nt_view *view)
{
	struct cli_page_layout_navigation *nav = (struct cli_page_layout_navigation *)view;
	struct nt_view_rect rect = nt_view_content_rect(view);
	int child = *nt_label_text(nav->subpage) != '\0';
	struct nt_text_style style = {
		.attributes = child && nav->back ? NT_STYLE_UNDERLINE : NT_STYLE_BOLD};
	int rc = nt_view_set_style(nav->page, style);
	if (!rc)
		rc = nt_view_set_focusable(nav->page, child && nav->back);

	struct nt_view *parts[] = {nav->home, nav->separator, nav->page, nav->branch,
				 nav->subpage};
	int x = rect.x;
	for (size_t i = 0; !rc && i < sizeof(parts) / sizeof(parts[0]); i++) {
		struct nt_view_size size = {0};
		if (i < 3 || child)
			rc = nt_view_measure(parts[i], 0, &size);

		int available = rect.x + rect.width - x;
		int width = size.width < available ? size.width : available;
		if (width < 0)
			width = 0;

		if (!rc)
			rc = nt_view_arrange(parts[i],
					     (struct nt_view_rect){x, rect.y, width, rect.height});

		x += width;
	}

	return rc;
}

static int cli_page_layout_rule_draw(struct nt_view *view, struct nt_canvas *canvas)
{
	(void)view;
	const struct nt_text_style dim = {.attributes = NT_STYLE_DIM};
	int rc = 0;
	for (int x = 0; !rc && x < canvas->width; x++)
		rc = nt_canvas_text_styled(canvas, x, 0, "─", &dim);

	return rc;
}

void cli_page_layout_build(struct nt_builder *builder, struct cli_page_layout *layout,
			   const char *name, const struct nt_view_class *body_class,
			   cli_page_layout_back_fn back, void *data)
{
	struct cli_page_layout_navigation *nav = NULL;
	static const struct nt_view_class navigation_class = {
		.superclass = &nt_view_base_class,
		.instance_size = sizeof(*nav),
		.layout = cli_page_layout_navigation_layout,
	};
	static const struct nt_view_class rule_class = {
		.superclass = &nt_view_base_class,
		.instance_size = sizeof(*layout->root),
		.draw = cli_page_layout_rule_draw,
	};
	const struct nt_view_options inset = {.padding = {1, 2, 1, 2}};
	const struct nt_view_options row = {.height = 1};
	const struct nt_view_options fill = {.grow = 1};
	const struct nt_view_options link = {
		.style = {.attributes = NT_STYLE_UNDERLINE, .foreground = {NT_COLOR_ANSI, 6}}};
	const struct nt_view_options dim = cli_panel_hint(1);
	const struct nt_view_options bold = cli_panel_heading(1);
	layout->root = nt_column_add(builder, NULL, 0, &inset);
	nav = (struct cli_page_layout_navigation *)nt_view_add(
		builder, layout->root, &navigation_class, &row);
	if (!nav)
		return;

	nav->back = back;
	nav->data = data;
	nav->home = nt_label_add(builder, &nav->base, "morph", &link);
	nav->separator = nt_label_add(builder, &nav->base, " / ", &dim);
	nav->page = nt_label_add(builder, &nav->base, name, &bold);
	nav->branch = nt_label_add(builder, &nav->base, " / ", &dim);
	nav->subpage = nt_label_add(builder, &nav->base, "", &bold);
	layout->subpage = nav->subpage;
	nt_view_add(builder, layout->root, &rule_class, &row);
	layout->body = nt_view_add(builder, layout->root, body_class, &fill);
	nt_view_add(builder, layout->root, &rule_class, &row);
	layout->footer = nt_column_add(builder, layout->root, 0, NULL);
	if (nt_builder_error(builder))
		return;

	struct nt_view *labels[] = {nav->home, nav->separator, nav->page, nav->branch,
				  nav->subpage};
	for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
		nt_builder_fail(builder, nt_label_set_wrap_mode(labels[i], NT_TEXT_WRAP_NONE));
		nt_builder_fail(builder, nt_view_set_selectable(labels[i], 0));
	}

	nt_builder_fail(builder, nt_view_set_focusable(nav->home, 1));
	nt_builder_fail(builder, nt_view_set_help_label(nav->home, "Return to chat"));
	struct nt_view *links[] = {nav->home, nav->page};
	for (size_t i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
		if (!nt_view_on(links[i], NT_VIEW_EVENT_CLICK, cli_page_layout_activate, nav) ||
		    !nt_view_on(links[i], NT_VIEW_EVENT_KEY, cli_page_layout_activate, nav))
			nt_builder_fail(builder, NB_ERR_NOMEM);
	}
}

void cli_page_layout_bind_subpage(struct nt_builder *builder, struct cli_page_layout *layout,
				  nt_compute_fn compute, void *data)
{
	nt_builder_fail(builder,
			nt_view_bind_compute(layout->subpage, NT_PROP_TEXT, compute, data));
}

int cli_page_layout_set_subpage(struct cli_page_layout *layout, const char *name)
{
	return nt_label_set_text(layout->subpage, name);
}
