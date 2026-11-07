#include <morph/tui.h>

struct sample_controls_page {
	struct morph_tui_view_controller base;
	struct morph_tui_view *status;
};

static void sample_controls_dialog_build(struct morph_tui_view_controller *controller,
					 struct morph_tui_builder *builder)
{
	struct morph_tui_view *root = morph_tui_widget_stack_add_borrow(
		builder, NULL, MORPH_TUI_WIDGET_STACK_VERTICAL, 1, NULL);

	morph_tui_widget_label_add_borrow(builder, root, "Selection", NULL);
	morph_tui_widget_label_add_borrow(builder, root, controller->data, NULL);
	controller->data = NULL;
	morph_tui_widget_label_add_borrow(builder, root,
					  "Escape returns to the same focused control.", NULL);
}

static void sample_controls_build(struct morph_tui_view_controller *controller,
				  struct morph_tui_builder *builder)
{
	struct sample_controls_page *page = (struct sample_controls_page *)controller;
	const char *items[] = {"Alpha", "Beta", "Gamma"};
	const struct morph_tui_view_options fill = {.grow = 1};
	struct morph_tui_view *root = morph_tui_widget_stack_add_borrow(
		builder, NULL, MORPH_TUI_WIDGET_STACK_VERTICAL, 1, NULL);

	morph_tui_widget_label_add_borrow(builder, root, "morph-tui controls", NULL);
	morph_tui_widget_label_add_borrow(
		builder, root,
		"Tab/Shift-Tab: focus; Enter: open; Esc: quit; q is text in the field", NULL);
	morph_tui_widget_text_field_add_borrow(builder, root, "", 1024, 1, NULL);
	morph_tui_widget_list_add_borrow(builder, root, items, 3, 2, &fill);
	page->status = morph_tui_widget_label_add_borrow(builder, root, "Ready", NULL);
}

static int sample_controls_action(struct morph_tui_view_controller *controller,
				  const struct morph_tui_widget_action *action)
{
	struct sample_controls_page *page = (struct sample_controls_page *)controller;

	if (action->type == MORPH_TUI_WIDGET_ACTION_CHANGED)
		return morph_tui_widget_label_set_text_copy(page->status, action->text);
	static const struct morph_tui_view_controller_ops dialog_ops = {
		.build = sample_controls_dialog_build,
	};
	const struct morph_tui_view_controller_spec dialog = {
		.ops = &dialog_ops,
		.data = (void *)action->text,
	};

	return morph_tui_view_controller_present_borrow(
		morph_tui_view_controller_application_borrow(controller), &dialog);
}

int main(void)
{
	struct sample_controls_page page;
	const struct morph_tui_view_controller_ops ops = {
		.build = sample_controls_build,
		.action = sample_controls_action,
	};
	const struct morph_tui_view_controller_spec root = {
		.ops = &ops,
		.size = sizeof(page),
		.quit_key = 'q',
	};
	struct morph_tui_application application = {0};

	return morph_tui_application_main(&application, &root);
}
