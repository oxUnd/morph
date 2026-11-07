#include <morph/tui.h>

struct sample_minimal_page {
	struct morph_tui_view_controller base;
	struct morph_tui_view *status;
};

static void sample_minimal_build(struct morph_tui_view_controller *controller,
				 struct morph_tui_builder *builder)
{
	struct sample_minimal_page *page = (struct sample_minimal_page *)controller;
	const char *items[] = {"Alpha", "Beta", "Gamma"};
	const struct morph_tui_view_options fill = {.grow = 1};
	struct morph_tui_view *root = morph_tui_widget_stack_add_borrow(
		builder, NULL, MORPH_TUI_WIDGET_STACK_VERTICAL, 1, NULL);

	morph_tui_widget_label_add_borrow(builder, root, "morph-tui minimal", NULL);
	morph_tui_widget_label_add_borrow(builder, root,
					  "Up/Down or j/k: move; Enter: select; q/Esc: quit", NULL);
	morph_tui_widget_list_add_borrow(builder, root, items, 3, 0, &fill);
	page->status = morph_tui_widget_label_add_borrow(builder, root, "Selected: none", NULL);
}

static int sample_minimal_action(struct morph_tui_view_controller *controller,
				 const struct morph_tui_widget_action *action)
{
	struct sample_minimal_page *page = (struct sample_minimal_page *)controller;

	if (action->type == MORPH_TUI_WIDGET_ACTION_ACTIVATE)
		return morph_tui_widget_label_set_text_copy(page->status, action->text);
	return 0;
}

int main(void)
{
	struct sample_minimal_page page;
	const struct morph_tui_view_controller_ops ops = {
		.build = sample_minimal_build,
		.action = sample_minimal_action,
	};
	const struct morph_tui_view_controller_spec root = {
		.ops = &ops,
		.size = sizeof(page),
		.quit_key = 'q',
	};
	struct morph_tui_application application = {0};

	return morph_tui_application_main(&application, &root);
}
