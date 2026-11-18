#include "sapi/cli/components/plan.h"
#include "sapi/cli/components/report.h"
#include "sapi/cli/presentation/plan.h"
#include "sapi/cli/presentation/style.h"

struct cli_plan_widget {
	struct cli_report report;
	cJSON *state;
};

static int cli_plan_widget_format(struct cli_report *report, nb_buf *out, int width)
{
	struct cli_plan_widget *widget = (struct cli_plan_widget *)report;
	const struct cli_plan_style style = {.columns = width + (int)CLI_CONTENT_RIGHT_PADDING,
					     .styled = report->styled,
					     .status_heading = 1};
	return cli_plan_append(out, widget->state, &style);
}

static void cli_plan_widget_destroy(struct nt_view *view)
{
	cJSON_Delete(((struct cli_plan_widget *)view)->state);
}

static const struct nt_view_class cli_plan_widget_class = {
	.superclass = &cli_report_class,
	.instance_size = sizeof(struct cli_plan_widget),
	.kind = "cli_plan_widget",
	.deinit = cli_plan_widget_destroy,
};

struct nt_view *cli_plan_add_borrow(struct nt_builder *builder, struct nt_view *parent,
				    const cJSON *state, int styled)
{
	if (!state) {
		nt_builder_fail(builder, NB_ERR_INVAL);
		return NULL;
	}

	const struct nt_view_options options = {
		.margin = {.top = 1}, .padding = {.right = (int)CLI_CONTENT_RIGHT_PADDING}};
	struct cli_plan_widget *widget = (struct cli_plan_widget *)cli_report_add(
		builder, parent, &cli_plan_widget_class, &options, styled, cli_plan_widget_format);
	if (!widget)
		return NULL;

	struct nt_view *view = &widget->report.base;
	widget->state = cJSON_Duplicate(state, 1);
	if (!widget->state)
		nt_builder_fail(builder, NB_ERR_NOMEM);

	return view;
}
