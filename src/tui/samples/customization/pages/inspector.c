#include "pages/inspector.h"
#include "widgets/counter.h"

struct sample_customization_page_inspector {
	struct morph_tui_view_controller base;
	struct morph_tui_view *status;
};

static void sample_customization_page_inspector_build(struct morph_tui_view_controller *base,
						      struct morph_tui_builder *builder)
{
	struct sample_customization_application *app = base->data;
	struct sample_customization_page_inspector *page =
		(struct sample_customization_page_inspector *)base;
	const struct morph_tui_view_options inset = {.padding = 1};
	const struct morph_tui_view_options row = {.height = 3};
	const struct morph_tui_view_options label = {.width = 12};
	struct morph_tui_view *root = morph_tui_widget_stack_create(
		builder, NULL, MORPH_TUI_WIDGET_STACK_VERTICAL, 1, &inset);

	morph_tui_widget_label_create(builder, root, "Inspector / custom page", NULL);
	morph_tui_widget_label_create(builder, root,
				      "Same model and custom widget, different layout", NULL);
	struct morph_tui_view *content = morph_tui_widget_stack_create(
		builder, root, MORPH_TUI_WIDGET_STACK_HORIZONTAL, 1, &row);

	morph_tui_widget_label_create(builder, content, "Shared value", &label);
	sample_customization_widget_counter_create(builder, content, 10, app->value, app->step);
	morph_tui_widget_label_create(builder, root, "Shared name (Enter saves)", NULL);
	morph_tui_widget_text_field_create(builder, root, morph_buf_cstr(&app->name), 128, 20,
					   NULL);
	page->status = morph_tui_widget_label_create(builder, root,
						     "Changes persist when returning", NULL);
	morph_tui_widget_label_create(
		builder, root, "Tab: focus | ?: help | Esc: return to the same dashboard control",
		NULL);
}

static int sample_customization_page_inspector_action(struct morph_tui_view_controller *base,
						      const struct morph_tui_widget_action *action)
{
	struct sample_customization_application *app = base->data;
	struct sample_customization_page_inspector *page =
		(struct sample_customization_page_inspector *)base;

	if (action->tag == 10) {
		app->value = sample_customization_widget_counter_value(action->sender);
		app->step = sample_customization_widget_counter_step(action->sender);
		return morph_tui_widget_label_set_text(page->status, action->text);
	}
	if (action->type == MORPH_TUI_WIDGET_ACTION_SUBMIT) {
		int rc = sample_customization_application_set_name(app, action->text);

		return rc ? rc
			  : morph_tui_widget_label_set_text(page->status,
							    "Name saved to shared model");
	}
	return 0;
}

struct morph_tui_view_controller_spec
sample_customization_page_inspector_spec(struct sample_customization_application *app)
{
	static const struct morph_tui_view_controller_ops methods = {
		.build = sample_customization_page_inspector_build,
		.action = sample_customization_page_inspector_action,
	};
	struct sample_customization_page_inspector page;

	return (struct morph_tui_view_controller_spec){
		.ops = &methods,
		.data = app,
		.size = sizeof(page),
	};
}
