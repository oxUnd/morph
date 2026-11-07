#include "pages/dashboard.h"
#include "pages/help.h"
#include "pages/inspector.h"
#include "widgets/counter.h"

#define SAMPLE_CUSTOMIZATION_COUNTER_TAG 10
#define SAMPLE_CUSTOMIZATION_MENU_TAG 30

struct sample_customization_page_dashboard {
	struct morph_tui_view_controller base;
	struct morph_tui_view *counter, *name, *status;
	unsigned name_revision;
};

static int sample_customization_page_dashboard_name(void *data,
						    const struct morph_tui_widget_action *action)
{
	struct sample_customization_page_dashboard *page = data;
	struct sample_customization_application *app = page->base.data;

	if (action->type != MORPH_TUI_WIDGET_ACTION_SUBMIT)
		return morph_tui_widget_label_set_text_copy(page->status,
							    "Editing draft; Enter saves the name");
	int rc = sample_customization_application_set_name(app, action->text);

	if (!rc) {
		page->name_revision = app->name_revision;
		rc = morph_tui_widget_label_set_text_copy(page->status, "Bound action: name saved");
	}
	return rc;
}

static void sample_customization_page_dashboard_build(struct morph_tui_view_controller *base,
						      struct morph_tui_builder *builder)
{
	struct sample_customization_page_dashboard *page =
		(struct sample_customization_page_dashboard *)base;
	struct sample_customization_application *app = base->data;
	const struct morph_tui_view_options inset = {.margin = {.top = 1, .bottom = 1},
						     .padding = {.left = 1, .right = 1}};
	const struct morph_tui_view_options fill = {.grow = 1};
	const char *choices[] = {"Inspect shared state", "Open help", "Reset counter"};
	struct morph_tui_view *root = morph_tui_widget_stack_add_borrow(
		builder, NULL, MORPH_TUI_WIDGET_STACK_VERTICAL, 1, &inset);

	morph_tui_widget_label_add_borrow(builder, root, "morph-tui customization / Dashboard",
					  NULL);
	morph_tui_widget_label_add_borrow(builder, root,
					  "Widget keys, page commands, and app shortcuts", NULL);
	page->counter = sample_customization_widget_counter_create(
		builder, root, SAMPLE_CUSTOMIZATION_COUNTER_TAG, app->value, app->step);
	page->name = morph_tui_widget_text_field_add_borrow(
		builder, root, morph_buf_cstr_borrow(&app->name), 128, 20, NULL);
	morph_tui_widget_list_add_borrow(builder, root, choices, 3, SAMPLE_CUSTOMIZATION_MENU_TAG,
					 &fill);
	page->status = morph_tui_widget_label_add_borrow(builder, root, "Dashboard active", NULL);
	morph_tui_widget_label_add_borrow(
		builder, root, "Tab: focus | r: reset | ?: help | Ctrl-L: redraw | Esc/q: quit",
		NULL);
	page->name_revision = app->name_revision;
	morph_tui_builder_fail(builder,
			       morph_tui_widget_set_action_borrow(
				       page->name, sample_customization_page_dashboard_name, page));
}

static int sample_customization_page_dashboard_appear(struct morph_tui_view_controller *base)
{
	struct sample_customization_page_dashboard *page =
		(struct sample_customization_page_dashboard *)base;
	struct sample_customization_application *app = base->data;
	int rc = sample_customization_widget_counter_set(page->counter, app->value, app->step);

	if (!rc && page->name_revision != app->name_revision) {
		rc = morph_tui_widget_text_field_set_text_copy(page->name,
							       morph_buf_cstr_borrow(&app->name));
		if (!rc)
			page->name_revision = app->name_revision;
	}
	return rc;
}

static int
sample_customization_page_dashboard_reset(struct sample_customization_page_dashboard *page,
					  const char *message)
{
	struct sample_customization_application *app = page->base.data;

	app->value = 0;
	int rc = sample_customization_widget_counter_set(page->counter, app->value, app->step);

	return rc ? rc : morph_tui_widget_label_set_text_copy(page->status, message);
}

static int sample_customization_page_dashboard_action(struct morph_tui_view_controller *base,
						      const struct morph_tui_widget_action *action)
{
	struct sample_customization_page_dashboard *page =
		(struct sample_customization_page_dashboard *)base;
	struct sample_customization_application *app = base->data;
	int rc = 0;

	if (action->tag == SAMPLE_CUSTOMIZATION_COUNTER_TAG) {
		app->value = sample_customization_widget_counter_value(action->sender);
		app->step = sample_customization_widget_counter_step(action->sender);
		rc = morph_tui_widget_label_set_text_copy(page->status, action->text);
		if (rc || action->type != MORPH_TUI_WIDGET_ACTION_ACTIVATE)
			return rc;
	} else if (action->type != MORPH_TUI_WIDGET_ACTION_ACTIVATE)
		return 0;
	else if (action->index == 1) {
		struct morph_tui_view_controller_spec help = sample_customization_page_help_spec();

		return morph_tui_view_controller_present_borrow(&app->base, &help);
	} else if (action->index == 2) {
		return sample_customization_page_dashboard_reset(page,
								 "List action: counter reset");
	}
	struct morph_tui_view_controller_spec inspector =
		sample_customization_page_inspector_spec(app);

	return morph_tui_view_controller_present_borrow(&app->base, &inspector);
}

static int sample_customization_page_dashboard_event(struct morph_tui_view_controller *base,
						     const struct morph_tui_event *event)
{
	struct sample_customization_page_dashboard *page =
		(struct sample_customization_page_dashboard *)base;

	if (event->type == MORPH_TUI_EVENT_MESSAGE) {
		const char *message = event->code == SAMPLE_CUSTOMIZATION_READY
					      ? "Launch message delivered to the page"
					      : "App shortcut: redraw requested";
		int rc = morph_tui_widget_label_set_text_copy(page->status, message);

		return rc ? rc : 1;
	}
	if ((event->type == MORPH_TUI_EVENT_KEY || event->type == MORPH_TUI_EVENT_TEXT) &&
	    event->key == 'r') {
		int rc = sample_customization_page_dashboard_reset(
			page, "Page shortcut: r reset counter");

		return rc ? rc : 1;
	}
	return 0;
}

struct morph_tui_view_controller_spec
sample_customization_page_dashboard_spec(struct sample_customization_application *app)
{
	static const struct morph_tui_view_controller_ops methods = {
		.build = sample_customization_page_dashboard_build,
		.action = sample_customization_page_dashboard_action,
		.event = sample_customization_page_dashboard_event,
		.appear = sample_customization_page_dashboard_appear,
	};
	struct sample_customization_page_dashboard page;

	return (struct morph_tui_view_controller_spec){
		.ops = &methods,
		.data = app,
		.size = sizeof(page),
		.quit_key = 'q',
	};
}
