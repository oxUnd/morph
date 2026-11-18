#include "sapi/cli/components/recap.h"
#include "sapi/cli/presentation/recap.h"
#include "sapi/cli/components/report.h"
#include "sapi/cli/presentation/style.h"
#include <nothing/tui/component_state.h>
#include <nb/err.h>

struct cli_recap_widget {
	struct cli_report report;
	struct nt_component_state *summary, *next;
};

static int cli_recap_widget_format(struct cli_report *report, nb_buf *out, int width)
{
	struct cli_recap_widget *widget = (struct cli_recap_widget *)report;
	return cli_recap_append(out, nt_component_state_get_string(widget->summary),
				nt_component_state_get_string(widget->next),
				width + (int)CLI_CONTENT_RIGHT_PADDING, report->styled);
}

static const struct nt_view_class cli_recap_widget_class = {
	.superclass = &cli_report_class,
	.instance_size = sizeof(struct cli_recap_widget),
	.kind = "cli_recap_widget",
};

struct nt_view *cli_recap_add_borrow(struct nt_builder *builder, struct nt_view *parent,
				     const char *summary, const char *next, int styled)
{
	if (!summary) {
		nt_builder_fail(builder, NB_ERR_INVAL);
		return NULL;
	}
	const struct nt_view_options options = {
		.margin = {.top = 1, .bottom = 1},
		.padding = {.right = (int)CLI_CONTENT_RIGHT_PADDING}};
	struct cli_recap_widget *widget = (struct cli_recap_widget *)cli_report_add(
		builder, parent, &cli_recap_widget_class, &options, styled,
		cli_recap_widget_format);
	if (!widget)
		return NULL;
	struct nt_view *view = &widget->report.base;
	int rc = nt_view_use_state_string(view, summary, &widget->summary);
	if (!rc)
		rc = nt_view_use_state_string(view, next ? next : "", &widget->next);
	nt_builder_fail(builder, rc);
	return view;
}
