#include "sapi/cli/pages/turn_details.h"

#include "sapi/cli/components/shortcut_hint.h"

#include "sapi/cli/application.h"
#include "sapi/cli/components/report.h"
#include "sapi/cli/components/page_layout.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/panel.h"
#include "sapi/cli/presentation/transcript.h"
#include "sapi/cli/presentation/transcript_details.h"
#include "sapi/cli/services/recap.h"

#include <nb/cleanup.h>
#include <nb/err.h>
#include <nothing/tui/application.h>
#include <nothing/tui/component_state.h>
#include <nothing/tui/input.h>
#include <nothing/tui/terminal.h>
#include <nothing/tui/view_controller.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/scroll_view.h>
#include <nothing/tui/views/stack_view.h>

struct cli_turn_details {
	struct nt_view_controller base;
	struct cli_page_layout layout;
	struct nt_view *counter, *scroll;
	struct cli_report *body;
	struct nt_component_state *raw;
	const struct cli_transcript_model *model;
	uint64_t revision;
	int visible;
};

int cli_turn_details_is_open(const struct cli_application *ctx)
{
	return ctx && ctx->details;
}

int cli_turn_details_is_visible(const struct cli_application *ctx)
{
	return ctx && ctx->details && ctx->details->visible;
}

static int cli_turn_details_document_format(struct cli_report *report, nb_buf *out, int width)
{
	struct cli_application *ctx = cli_application_from_view(&report->base);
	struct cli_turn_details *page =
		(struct cli_turn_details *)nt_view_controller(&report->base);
	return cli_transcript_details_append(out, ctx->transcript, width,
		cli_output_color_enabled(ctx), nt_component_state_get_bool(page->raw));
}

static int cli_turn_details_mode(void *data, struct nt_value *value)
{
	return nt_value_set_string(value, nt_component_state_get_bool(data) ? " · raw JSON" : "");
}

static int cli_turn_details_shortcuts(void *data, struct nt_value *value)
{
	return nt_value_set_string(value, nt_component_state_get_bool(data)
		? "R formatted · Ctrl+T / Esc back · PgUp/PgDn scroll · End follow"
		: "R raw JSON · Ctrl+T / Esc back · PgUp/PgDn scroll · End follow");
}

static int cli_turn_details_set_raw(struct cli_turn_details *page, int raw)
{
	int rc = nt_component_state_set_bool(page->raw, raw != 0);
	if (!rc)
		cli_report_invalidate(page->body,
			cli_output_color_enabled(cli_application_from_controller(&page->base)));

	return rc;
}

static int cli_turn_details_refresh(struct cli_turn_details *page)
{
	struct cli_application *ctx = cli_application_from_controller(&page->base);
	const struct cli_transcript_model *model = ctx->transcript;
	uint64_t revision = model ? model->revision : 0;
	if (page->model == model && page->revision == revision)
		return 0;

	nb_buf counter NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!nb_buf_printf(&counter, "  · %zu calls", model ? model->tools.len : 0))
		return nb_buf_status(&counter);

	int rc = nt_label_set_text(page->counter, nb_buf_cstr(&counter));
	if (rc)
		return rc;

	page->model = model;
	page->revision = revision;
	cli_report_invalidate(page->body, cli_output_color_enabled(ctx));
	return rc;
}

static void cli_turn_details_build(struct nt_view_controller *controller,
				   struct nt_builder *builder)
{
	struct cli_turn_details *page = (struct cli_turn_details *)controller;
	struct cli_application *ctx = cli_application_from_controller(controller);
	cli_page_layout_build(builder, &page->layout, "turn details", &nt_stack_view_class,
			      NULL, NULL);
	struct nt_view *root = page->layout.body;
	nt_builder_fail(builder, nt_view_use_state_bool(root, false, &page->raw));
	const struct nt_view_options header = {.padding = {.bottom = 1}};
	struct nt_view *heading =
		nt_stack_view_add(builder, root, NT_STACK_VIEW_HORIZONTAL, 0, &header);
	struct nt_view_options title = {0}, counter = {0};
	if (cli_output_color_enabled(ctx)) {
		title = cli_panel_heading(NT_AUTO);
		counter = cli_panel_hint(NT_AUTO);
	}

	nt_label_add(builder, heading, "Turn details", &title);
	page->counter = nt_label_add(builder, heading, "", &counter);
	struct nt_view *mode = nt_label_add(builder, heading, "", &counter);
	if (mode)
		nt_builder_fail(builder, nt_view_bind_compute(mode, NT_PROP_TEXT,
			cli_turn_details_mode, page->raw));

	const struct nt_view_options fill = {.grow = 1};
	page->scroll = nt_scroll_view_add(builder, root, &fill);
	nt_builder_fail(builder, nt_scroll_view_set_scrollbars(page->scroll, 0));
	nt_builder_fail(builder, nt_scroll_view_follow(page->scroll, 1));
	const struct nt_view_options document_options = {.width = NT_FILL};
	page->body =
		cli_report_add(builder, page->scroll, &cli_report_class, &document_options,
			       cli_output_color_enabled(ctx), cli_turn_details_document_format);

	const struct nt_view_options footer = cli_panel_hint(NT_AUTO);
	struct nt_view *shortcuts = cli_shortcut_hint_add(builder, page->layout.footer, "", &footer);
	if (shortcuts)
		nt_builder_fail(builder, nt_view_bind_compute(shortcuts, NT_PROP_TEXT,
			cli_turn_details_shortcuts, page->raw));
}

static int cli_turn_details_event(struct nt_view_controller *controller,
				  const struct nt_event *event)
{
	struct cli_turn_details *page = (struct cli_turn_details *)controller;
	if (event->type == NT_EVENT_KEY && event->action == NT_KEY_ACTION_RELEASE)
		return 0;

	int key = event->type == NT_EVENT_TEXT && event->text_length == 1
		? (unsigned char)event->text[0] : event->type == NT_EVENT_KEY ? event->key : 0;
	if (!event->pasting && (key == 'r' || key == 'R')) {
		if (event->action != NT_KEY_ACTION_PRESS)
			return 1;

		int rc = cli_turn_details_set_raw(page, !nt_component_state_get_bool(page->raw));
		return rc ? rc : 1;
	}

	if (event->type == NT_EVENT_KEY && !event->pasting) {
		if (event->key == 20 && event->action != NT_KEY_ACTION_PRESS)
			return 1;

		if (event->key == 20 || event->key == 27 || event->key == NT_KEY_ESCAPE ||
		    event->key == 3)
			return nt_view_controller_dismiss(controller, 0);

		if (event->key == NT_KEY_FOCUS_IN || event->key == NT_KEY_FOCUS_OUT)
			cli_recap_focus(cli_application_from_controller(controller),
					event->key == NT_KEY_FOCUS_IN, cli_recap_now());

		int steps = event->key == NT_KEY_WHEEL_UP || event->key == NT_KEY_WHEEL_DOWN ? 3 : 1;
		int rc = 0;
		for (int i = 0; i < steps && rc >= 0; i++)
			rc = nt_scroll_view_handle_key(page->scroll, event->key);

		if (steps > 1 && rc >= 0)
			return 1;

		if (rc)
			return rc;
	}

	if (event->type == NT_EVENT_WAKE || event->type == NT_EVENT_TICK ||
	    event->type == NT_EVENT_RESIZE)
		return cli_turn_details_refresh(page);

	return 0;
}

static void cli_turn_details_appear(struct nt_view_controller *controller)
{
	struct cli_turn_details *page = (struct cli_turn_details *)controller;
	page->visible = 1;
	int rc = cli_turn_details_refresh(page);
	if (rc)
		nt_application_stop(nt_view_controller_application(controller), rc);
}

static void cli_turn_details_disappear(struct nt_view_controller *controller)
{
	((struct cli_turn_details *)controller)->visible = 0;
}

static void cli_turn_details_close(struct nt_view_controller *controller)
{
	struct cli_application *ctx = cli_application_from_controller(controller);
	if (ctx && ctx->details == (struct cli_turn_details *)controller)
		ctx->details = NULL;
}

void cli_turn_details_toggle(struct cli_application *ctx)
{
	struct nt_application *app = cli_application_tui_borrow(ctx);
	if (!app || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;

	if (ctx->details) {
		(void)nt_view_controller_dismiss(&ctx->details->base, 0);
		return;
	}

	if (!ctx->transcript && cli_transcript_model_create(&ctx->transcript))
		return;

	static const struct nt_view_controller_class type = {
		.superclass = &cli_application_panel_class,
		.instance_size = sizeof(struct cli_turn_details),
		.load_view = cli_turn_details_build,
		.handle_event = cli_turn_details_event,
		.view_will_appear = cli_turn_details_appear,
		.view_will_disappear = cli_turn_details_disappear,
		.deinit = cli_turn_details_close,
	};

	struct nt_view_controller *owner NB_CLEANUP(cli_application_controller_cleanup) = NULL;
	if (nt_view_controller_create(&type, &owner))
		return;

	struct cli_turn_details *page = (struct cli_turn_details *)owner;
	if (!cli_application_show_page(ctx, &owner, 250))
		ctx->details = page;
}
