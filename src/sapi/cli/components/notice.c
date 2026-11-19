#include "sapi/cli/components/notice.h"
#include "sapi/cli/components/report.h"
#include "sapi/cli/presentation/notice.h"
#include "sapi/cli/presentation/style.h"
#include <nothing/tui/component_state.h>

struct cli_notice_widget {
	struct cli_report report;
	enum cli_notice_status status;
	int tree_branch;
	struct nt_component_state *title, *detail, *footer;
};

static int cli_notice_widget_format(struct cli_report *report, nb_buf *out, int width)
{
	struct cli_notice_widget *widget = (struct cli_notice_widget *)report;
	const struct cli_notice notice = {.status = widget->status,
					  .tree_branch = widget->tree_branch,
					  .title = nt_component_state_get_string(widget->title),
					  .detail = nt_component_state_get_string(widget->detail),
					  .footer = nt_component_state_get_string(widget->footer)};
	return cli_notice_append(out, &notice, width + (int)CLI_CONTENT_RIGHT_PADDING,
				 report->styled);
}

static const struct nt_view_class cli_notice_widget_class = {
	.superclass = &cli_report_class,
	.instance_size = sizeof(struct cli_notice_widget),
	.kind = "cli_notice_widget",
};

struct nt_view *cli_notice_add_borrow(struct nt_builder *builder, struct nt_view *parent,
				      const struct cli_notice *notice, int styled)
{
	if (!notice || !notice->title) {
		nt_builder_fail(builder, NB_ERR_INVAL);
		return NULL;
	}

	const struct nt_view_options options = {
		.width = NT_FILL,
		.margin = {.top = 1}, .padding = {.right = (int)CLI_CONTENT_RIGHT_PADDING}};
	struct cli_notice_widget *widget = (struct cli_notice_widget *)cli_report_add(
		builder, parent, &cli_notice_widget_class, &options, styled,
		cli_notice_widget_format);
	if (!widget)
		return NULL;

	struct nt_view *view = &widget->report.base;
	widget->status = notice->status;
	widget->tree_branch = notice->tree_branch;
	int rc = nt_view_use_state_string(view, notice->title, &widget->title);
	if (!rc)
		rc = nt_view_use_state_string(view, notice->detail ? notice->detail : "",
					      &widget->detail);

	if (!rc)
		rc = nt_view_use_state_string(view, notice->footer ? notice->footer : "",
					      &widget->footer);

	nt_builder_fail(builder, rc);
	return view;
}
