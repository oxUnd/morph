#include "pages/help.h"

static void sample_customization_page_help_build(struct morph_tui_view_controller *base,
						 struct morph_tui_builder *builder)
{
	const struct morph_tui_view_options inset = {.padding = 1};
	const struct morph_tui_view_options fill = {.grow = 1};
	struct morph_tui_view *root = morph_tui_widget_stack_create(
		builder, NULL, MORPH_TUI_WIDGET_STACK_VERTICAL, 1, &inset);

	(void)base;
	morph_tui_widget_label_create(builder, root, "Help / interaction routing", NULL);
	morph_tui_widget_text_view_create(
		builder, root,
		"1. Custom Widget: the counter owns drawing, +/- keys, and Space.\n"
		"   Enter emits an ACTIVATE action to its page.\n\n"
		"2. Custom Page: Dashboard handles r to reset the shared model.\n"
		"   Inspector composes another layout and edits the same model.\n\n"
		"3. Custom Interaction: the name field has a bound submit action.\n"
		"   Unhandled ? reaches the Application and opens this help page.\n"
		"   Ctrl-L posts an application message to request a redraw.\n\n"
		"Text fields consume printable text first. Typing r, q, or ? there\n"
		"edits text rather than invoking page or application shortcuts.\n"
		"Pasting cannot activate controls or page/application commands.\n\n"
		"Escape returns with focus, selection, and drafts preserved.\n"
		"View state is restored from the model only after explicit changes.",
		&fill);
	morph_tui_widget_label_create(builder, root, "Up/Down: scroll | Esc/q: return", NULL);
}

static int sample_customization_page_help_event(struct morph_tui_view_controller *base,
						const struct morph_tui_event *event)
{
	(void)base;
	return (event->type == MORPH_TUI_EVENT_KEY || event->type == MORPH_TUI_EVENT_TEXT) &&
	       event->key == '?';
}

struct morph_tui_view_controller_spec sample_customization_page_help_spec(void)
{
	static const struct morph_tui_view_controller_ops methods = {
		.build = sample_customization_page_help_build,
		.event = sample_customization_page_help_event,
	};

	return (struct morph_tui_view_controller_spec){.ops = &methods, .quit_key = 'q'};
}
