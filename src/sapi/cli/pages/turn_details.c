#include "sapi/cli/pages/turn_details.h"
#include "sapi/cli/presentation/panel.h"
#include <nb/err.h>
#include "sapi/cli/application.h"
#include "sapi/cli/components/report.h"
#include "sapi/cli/presentation/transcript.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/services/recap.h"
#include <nb/cleanup.h>
#include <nothing/tui/application.h>
#include <nothing/tui/input.h>
#include <nothing/tui/terminal.h>
#include <nothing/tui/view_controller.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/scroll_view.h>
#include <nothing/tui/views/stack_view.h>

#include <stdlib.h>

struct cli_turn_details {
	struct cli_application *ctx;
	struct nt_view_controller *controller;
	struct nt_view *counter, *scroll;
	struct cli_report *body;
	const struct cli_transcript_model *model;
	uint64_t revision;
	int open, visible;
};

struct cli_turn_details_document {
	struct cli_report report;
	struct cli_turn_details *page;
};

static int cli_turn_details_prepare(struct cli_application *ctx)
{
	if (!ctx->details) {
		ctx->details = calloc(1, sizeof(*ctx->details));
		if (!ctx->details)
			return NB_ERR_NOMEM;

		ctx->details->ctx = ctx;
	}

	return 0;
}

int cli_turn_details_is_open(const struct cli_application *ctx)
{
	return ctx && ctx->details && ctx->details->open;
}

int cli_turn_details_is_visible(const struct cli_application *ctx)
{
	return ctx && ctx->details && ctx->details->visible;
}

static int cli_turn_details_document_format(struct cli_report *report, nb_buf *out, int width)
{
	struct cli_turn_details *page = ((struct cli_turn_details_document *)report)->page;
	(void)width;
	return cli_transcript_render_details(page->ctx, page->ctx->transcript, out);
}

static int cli_turn_details_refresh(struct cli_turn_details *page)
{
	if (!page->controller)
		return 0;

	const struct cli_transcript_model *model = page->ctx->transcript;
	uint64_t revision = model ? model->revision : 0;
	if (page->model == model && page->revision == revision)
		return 0;

	nb_buf counter NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_printf(&counter, "  · %zu calls",
				page->ctx->transcript ? page->ctx->transcript->tools.len : 0)
			  ? 0
			  : NB_ERR_NOMEM);
	if (!rc)
		rc = nt_label_set_text(page->counter, nb_buf_cstr(&counter));

	if (rc)
		return rc;

	page->model = model;
	page->revision = revision;
	cli_report_invalidate(page->body, cli_output_color_enabled(page->ctx));
	return rc;
}

static void cli_turn_details_build(struct nt_view_controller *controller,
				   struct nt_builder *builder)
{
	struct cli_turn_details *page = controller->data;
	page->controller = controller;
	page->model = NULL;
	struct nt_view *root = nt_stack_view_add(builder, NULL, NT_STACK_VIEW_VERTICAL, 0, NULL);
	const struct nt_view_options header = {.padding = {.left = 2, .bottom = 1}};
	struct nt_view *heading =
		nt_stack_view_add(builder, root, NT_STACK_VIEW_HORIZONTAL, 0, &header);
	struct nt_view_options title = {0}, counter = {0};
	if (cli_output_color_enabled(page->ctx)) {
		title = cli_panel_heading(NT_AUTO);
		counter = cli_panel_hint(NT_AUTO);
	}

	nt_label_add(builder, heading, "Turn details", &title);
	page->counter = nt_label_add(builder, heading, "", &counter);
	const struct nt_view_options fill = {.grow = 1};
	page->scroll = nt_scroll_view_add(builder, root, &fill);
	nt_builder_fail(builder, nt_scroll_view_set_scrollbars(page->scroll, 0));
	nt_builder_fail(builder, nt_scroll_view_follow(page->scroll, 1));
	static const struct nt_view_class document_type = {
		.superclass = &cli_report_class,
		.instance_size = sizeof(struct cli_turn_details_document),
		.kind = "turn_details_document"};
	const struct nt_view_options document_options = {.width = NT_FILL};
	page->body = cli_report_add(builder, page->scroll, &document_type, &document_options,
				    cli_output_color_enabled(page->ctx),
				    cli_turn_details_document_format);
	if (page->body)
		((struct cli_turn_details_document *)page->body)->page = page;

	const struct nt_view_options footer = {.margin = {.top = 1},
					       .padding = {.left = 2, .right = 1}};
	nt_label_add(builder, root, "Ctrl+T / Esc back · PgUp/PgDn scroll · End follow", &footer);
}

static int cli_turn_details_event(struct nt_view_controller *controller,
				  const struct nt_event *event)
{
	struct cli_turn_details *page = controller->data;
	if (event->type == NT_EVENT_KEY && !event->pasting) {
		if (event->key == 20 || event->key == 27 || event->key == NT_KEY_ESCAPE ||
		    event->key == 3)
			return nt_view_controller_dismiss(controller, 0);

		if (event->key == NT_KEY_FOCUS_IN || event->key == NT_KEY_FOCUS_OUT)
			cli_recap_focus(page->ctx, event->key == NT_KEY_FOCUS_IN, cli_recap_now());

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
	struct cli_turn_details *page = controller->data;
	page->visible = 1;
	int rc = cli_turn_details_refresh(page);
	if (rc)
		nt_application_stop(&page->ctx->base, rc);
}

static void cli_turn_details_disappear(struct nt_view_controller *controller)
{
	((struct cli_turn_details *)controller->data)->visible = 0;
}

static void cli_turn_details_complete(void *data, int result)
{
	(void)data;
	(void)result;
}

static void cli_turn_details_close(struct nt_view_controller *controller)
{
	struct cli_turn_details *page = controller->data;
	if (!page)
		return;

	page->open = 0;
	page->visible = 0;
	page->controller = NULL;
	page->counter = page->scroll = NULL;
	page->body = NULL;
	cli_application_panel_complete(page->ctx, page, cli_turn_details_complete,
				       nt_view_controller_result(controller));
}

void cli_turn_details_toggle(struct cli_application *ctx)
{
	struct nt_application *app = cli_application_tui_borrow(ctx);
	if (!app || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;

	if (cli_turn_details_is_open(ctx)) {
		(void)nt_view_controller_dismiss(ctx->details->controller, 0);
		return;
	}

	if (cli_turn_details_prepare(ctx))
		return;

	if (!ctx->transcript && cli_transcript_model_create(&ctx->transcript))
		return;

	static const struct nt_view_controller_class type = {
		.superclass = &nt_view_controller_base_class,
		.instance_size = sizeof(struct nt_view_controller),
		.load_view = cli_turn_details_build,
		.handle_event = cli_turn_details_event,
		.view_will_appear = cli_turn_details_appear,
		.view_will_disappear = cli_turn_details_disappear,
		.deinit = cli_turn_details_close};
	if (!cli_application_present_page(app, &type, ctx->details, 250))
		ctx->details->open = 1;
}

void cli_turn_details_cleanup(struct cli_application *ctx)
{
	if (!ctx || !ctx->details)
		return;

	free(ctx->details);
	ctx->details = NULL;
}
