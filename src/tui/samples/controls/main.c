#include <morph/tui/core/runner.h>
#include <morph/tui/widgets/label.h>
#include <morph/tui/widgets/list.h>
#include <morph/tui/widgets/stack.h>
#include <morph/tui/widgets/text_field.h>

struct sample_controls_page {
	struct morph_tui_core_view_controller base;
	struct morph_tui_core_view *status;
};

static void sample_controls_dialog_build(struct morph_tui_core_view_controller *controller,
					 struct morph_tui_core_builder *builder)
{
	struct morph_tui_core_view *root = morph_tui_widget_stack_create(
		builder, NULL, MORPH_TUI_WIDGET_STACK_VERTICAL, 1, NULL);

	morph_tui_widget_label_create(builder, root, "Selection", NULL);
	morph_tui_widget_label_create(builder, root, controller->data, NULL);
	controller->data = NULL;
	morph_tui_widget_label_create(builder, root, "Escape returns to the same focused control.",
				      NULL);
}

static void sample_controls_build(struct morph_tui_core_view_controller *controller,
				  struct morph_tui_core_builder *builder)
{
	struct sample_controls_page *page = (struct sample_controls_page *)controller;
	const char *items[] = {"Alpha", "Beta", "Gamma"};
	const struct morph_tui_core_view_options fill = {.grow = 1};
	struct morph_tui_core_view *root = morph_tui_widget_stack_create(
		builder, NULL, MORPH_TUI_WIDGET_STACK_VERTICAL, 1, NULL);

	morph_tui_widget_label_create(builder, root, "morph-tui controls", NULL);
	morph_tui_widget_label_create(
		builder, root,
		"Tab/Shift-Tab: focus; Enter: open; Esc: quit; q is text in the field", NULL);
	morph_tui_widget_text_field_create(builder, root, "", 1024, 1, NULL);
	morph_tui_widget_list_view_create(builder, root, items, 3, 2, &fill);
	page->status = morph_tui_widget_label_create(builder, root, "Ready", NULL);
}

static int sample_controls_action(struct morph_tui_core_view_controller *controller,
				  const struct morph_tui_widget_action *action)
{
	struct sample_controls_page *page = (struct sample_controls_page *)controller;

	if (action->type == MORPH_TUI_WIDGET_ACTION_CHANGED)
		return morph_tui_widget_label_set_text(page->status, action->text);
	static const struct morph_tui_core_view_controller_ops dialog_ops = {
		.build = sample_controls_dialog_build,
	};
	const struct morph_tui_core_view_controller_spec dialog = {
		.ops = &dialog_ops,
		.data = (void *)action->text,
	};

	return morph_tui_core_view_controller_present(
		morph_tui_core_view_controller_app(controller), &dialog);
}

int main(void)
{
	struct sample_controls_page page;
	const struct morph_tui_core_view_controller_ops ops = {
		.build = sample_controls_build,
		.action = sample_controls_action,
	};
	const struct morph_tui_core_view_controller_spec root = {
		.ops = &ops,
		.size = sizeof(page),
		.quit_key = 'q',
	};
	struct morph_tui_core_application application = {0};

	return morph_tui_core_application_main(&application, &root);
}
