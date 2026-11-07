#include "application.h"
#include "pages/help.h"

static int sample_customization_application_launch(struct morph_tui_application *base)
{
	struct sample_customization_application *app =
		(struct sample_customization_application *)base;
	int rc = morph_buf_init(&app->name, 128);

	app->value = 3;
	app->step = 1;
	if (!rc)
		rc = morph_buf_puts(&app->name, "Ada");
	if (!rc)
		rc = morph_tui_application_post(base, SAMPLE_CUSTOMIZATION_READY);
	return rc;
}

static int sample_customization_application_event(struct morph_tui_application *base,
						  const struct morph_tui_event *event)
{
	if (event->type != MORPH_TUI_EVENT_KEY && event->type != MORPH_TUI_EVENT_TEXT)
		return 0;
	int rc;

	if (event->key == '?') {
		struct morph_tui_view_controller_spec help = sample_customization_page_help_spec();

		rc = morph_tui_view_controller_present_borrow(base, &help);
	} else if (event->key == 12) {
		morph_tui_application_invalidate(base, MORPH_TUI_INVALIDATE_DISPLAY);
		rc = morph_tui_application_post(base, SAMPLE_CUSTOMIZATION_REDRAW);
	} else
		return 0;
	return rc ? rc : 1;
}

static void sample_customization_application_terminate(struct morph_tui_application *base,
						       int result)
{
	struct sample_customization_application *app =
		(struct sample_customization_application *)base;

	(void)result;
	morph_buf_cleanup(&app->name);
}

const struct morph_tui_application_ops *sample_customization_application_methods(void)
{
	static const struct morph_tui_application_ops methods = {
		.launch = sample_customization_application_launch,
		.event = sample_customization_application_event,
		.terminate = sample_customization_application_terminate,
	};

	return &methods;
}

int sample_customization_application_set_name(struct sample_customization_application *app,
					      const char *name)
{
	morph_buf_reset(&app->name);
	int rc = morph_buf_puts(&app->name, name);

	if (!rc)
		app->name_revision++;
	return rc;
}
