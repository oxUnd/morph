#include "sapi/cli/presentation/turn_details.h"
#include "sapi/cli/application.h"
#include "sapi/cli/widgets/transcript.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/services/recap.h"
#include "util/cleanup.h"
#include <morph/tui/application.h>
#include <morph/tui/input.h>
#include <morph/tui/terminal.h>
#include <morph/tui/view_controller.h>
#include <morph/tui/widgets/label.h>
#include <morph/tui/widgets/scroll.h>
#include <morph/tui/widgets/stack.h>
#include <morph/tui/widgets/text_view.h>

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct cli_turn_details {
	struct cli_application *ctx;
	struct morph_tui_view_controller *controller;
	struct morph_tui_view *counter, *scroll, *body;
	char *rendered;
	int open, visible, columns, dirty;
};

struct cli_turn_details_document {
	struct morph_tui_widget base;
	struct cli_turn_details *page;
	struct morph_tui_view *text;
};

static int cli_turn_details_prepare(struct cli_application *ctx)
{
	if (!ctx->details) {
		ctx->details = calloc(1, sizeof(*ctx->details));
		if (!ctx->details)
			return -ENOMEM;
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

static int cli_turn_details_document_measure(struct morph_tui_view *view, int width,
					    struct morph_tui_view_size *size)
{
	struct cli_turn_details_document *document = (struct cli_turn_details_document *)view;
	struct cli_turn_details *page = document->page;
	if (page->dirty || page->columns != width) {
		morph_buf_t rendered MORPH_CLEANUP(morph_buf_cleanup) = {0};
		int rc = cli_transcript_render_details(page->ctx, page->ctx->transcript, &rendered);
		/* A terminal newline ends its row; it does not add another document row. */
		if (!rc && rendered.len && rendered.data[rendered.len - 1] == '\n')
			rendered.data[--rendered.len] = '\0';
		if (!rc && (!page->rendered || strcmp(page->rendered,
			morph_buf_cstr_borrow(&rendered)))) {
			rc = cli_messages_set_rendered_view(
				document->text, morph_buf_cstr_borrow(&rendered),
				cli_output_color_enabled(page->ctx));
			if (!rc) {
				free(page->rendered);
				page->rendered = morph_buf_detach(&rendered);
			}
		}
		if (rc)
			return rc;
		page->columns = width;
		page->dirty = 0;
	}
	return morph_tui_view_measure(document->text, width, size);
}

static int cli_turn_details_document_layout(struct morph_tui_view *view)
{
	return morph_tui_view_arrange(((struct cli_turn_details_document *)view)->text,
		morph_tui_view_content_rect(view));
}

static int cli_turn_details_refresh(struct cli_turn_details *page)
{
	if (!page->controller)
		return 0;
	morph_buf_t counter MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_buf_printf(&counter, "  · %zu calls",
		page->ctx->transcript ? page->ctx->transcript->tools.nelts : 0);
	if (!rc)
		rc = morph_tui_widget_label_set_text_copy(page->counter,
			morph_buf_cstr_borrow(&counter));
	page->dirty = 1;
	morph_tui_view_invalidate(page->body, 1);
	return rc;
}

static void cli_turn_details_scroll(struct cli_turn_details *page, int scroll)
{
	if (scroll == INT_MIN)
		(void)morph_tui_widget_scroll_handle_key(page->scroll, MORPH_TUI_KEY_HOME);
	else if (scroll == INT_MAX)
		(void)morph_tui_widget_scroll_handle_key(page->scroll, MORPH_TUI_KEY_END);
	else {
		int key = scroll < 0 ? MORPH_TUI_KEY_UP : MORPH_TUI_KEY_DOWN;
		unsigned steps = scroll < 0 ? (unsigned)(-(scroll + 1)) + 1u : (unsigned)scroll;
		for (unsigned i = 0; i < steps; i++)
			(void)morph_tui_widget_scroll_handle_key(page->scroll, key);
	}
}

void cli_turn_details_render(struct cli_application *ctx, int scroll)
{
	if (!ctx || cli_turn_details_prepare(ctx))
		return;
	struct cli_turn_details *page = ctx->details;
	if (page->controller) {
		if (!cli_turn_details_refresh(page)) {
			cli_turn_details_scroll(page, scroll);
			(void)morph_tui_application_render(cli_application_tui_borrow(ctx));
		}
		return;
	}
	/* Stateless callers request a transcript snapshot outside the interactive page. */
	morph_buf_t rendered MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int visible = page->visible;
	page->visible = 1;
	int rc = cli_transcript_render_details(ctx, ctx->transcript, &rendered);
	if (!rc)
		(void)cli_output_printf(ctx, "%s", morph_buf_cstr_borrow(&rendered));
	page->visible = visible;
}

static void cli_turn_details_build(struct morph_tui_view_controller *controller,
				   struct morph_tui_builder *builder)
{
	struct cli_turn_details *page = controller->data;
	page->controller = controller;
	page->columns = -1;
	page->dirty = 1;
	free(page->rendered);
	page->rendered = NULL;
	struct morph_tui_view *root = morph_tui_widget_stack_add_borrow(builder, NULL,
		MORPH_TUI_WIDGET_STACK_VERTICAL, 0, NULL);
	const struct morph_tui_view_options header = {.padding = {.left = 2, .bottom = 1}};
	struct morph_tui_view *heading = morph_tui_widget_stack_add_borrow(builder, root,
		MORPH_TUI_WIDGET_STACK_HORIZONTAL, 0, &header);
	struct morph_tui_view_options title = {0}, counter = {0};
	if (cli_output_color_enabled(page->ctx)) {
		title.style.attributes = MORPH_TUI_STYLE_BOLD;
		counter.style.attributes = MORPH_TUI_STYLE_DIM;
	}
	morph_tui_widget_label_add_borrow(builder, heading, "Turn details", &title);
	page->counter = morph_tui_widget_label_add_borrow(builder, heading, "", &counter);
	const struct morph_tui_view_options fill = {.grow = 1};
	page->scroll = morph_tui_widget_scroll_add_borrow(builder, root, &fill);
	static const struct morph_tui_view_ops document_ops = {
		.measure = cli_turn_details_document_measure,
		.layout = cli_turn_details_document_layout};
	struct cli_turn_details_document *document =
		(struct cli_turn_details_document *)morph_tui_widget_add_borrow(builder,
			page->scroll, sizeof(*document), &document_ops, NULL);
	if (document) {
		document->page = page;
		page->body = &document->base.view;
		document->text = morph_tui_widget_text_view_add_borrow(
			builder, page->body, "", NULL);
		if (document->text) {
			morph_tui_view_set_focusable(document->text, 0);
			morph_tui_builder_fail(builder,
				morph_tui_widget_text_view_set_content_sizing(document->text, 1));
		}
	}
	const struct morph_tui_view_options footer = {
		.margin = {.top = 1}, .padding = {.left = 2, .right = 1}};
	morph_tui_widget_label_add_borrow(builder, root,
		"Ctrl+T / Esc back · PgUp/PgDn scroll · End follow", &footer);
}

static int cli_turn_details_event(struct morph_tui_view_controller *controller,
				  const struct morph_tui_event *event)
{
	struct cli_turn_details *page = controller->data;
	if (event->type == MORPH_TUI_EVENT_KEY && !event->pasting) {
		if (event->key == 20 || event->key == 27 || event->key == 3)
			return morph_tui_view_controller_dismiss(controller, 0);
		if (event->key == MORPH_TUI_KEY_FOCUS_IN || event->key == MORPH_TUI_KEY_FOCUS_OUT)
			cli_recap_focus(page->ctx, event->key == MORPH_TUI_KEY_FOCUS_IN,
				cli_recap_now());
		if (event->key == MORPH_TUI_KEY_WHEEL_UP ||
		    event->key == MORPH_TUI_KEY_WHEEL_DOWN) {
			cli_turn_details_scroll(
				page, event->key == MORPH_TUI_KEY_WHEEL_UP ? -3 : 3);
			return 1;
		}
		int rc = morph_tui_widget_scroll_handle_key(page->scroll, event->key);
		if (rc)
			return rc;
	}
	if (event->type == MORPH_TUI_EVENT_WAKE || event->type == MORPH_TUI_EVENT_TICK ||
	    event->type == MORPH_TUI_EVENT_RESIZE)
		return cli_turn_details_refresh(page);
	return 0;
}

static int cli_turn_details_appear(struct morph_tui_view_controller *controller)
{
	struct cli_turn_details *page = controller->data;
	page->visible = 1;
	return cli_turn_details_refresh(page);
}

static void cli_turn_details_disappear(struct morph_tui_view_controller *controller)
{
	((struct cli_turn_details *)controller->data)->visible = 0;
}

static void cli_turn_details_complete(void *data, int result)
{
	(void)data;
	(void)result;
}

static void cli_turn_details_close(struct morph_tui_view_controller *controller, int result)
{
	struct cli_turn_details *page = controller->data;
	page->open = 0;
	page->visible = 0;
	page->controller = NULL;
	page->counter = page->scroll = page->body = NULL;
	cli_application_panel_complete(page->ctx, page, cli_turn_details_complete, result);
}

void cli_turn_details_toggle(struct cli_application *ctx)
{
	struct morph_tui_application *app = cli_application_tui_borrow(ctx);
	if (!app || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	if (cli_turn_details_is_open(ctx)) {
		(void)morph_tui_application_dismiss_controller(app, 0);
		return;
	}
	if (cli_turn_details_prepare(ctx))
		return;
	if (!ctx->transcript && cli_transcript_model_create(&ctx->transcript))
		return;
	static const struct morph_tui_view_controller_ops ops = {
		.build = cli_turn_details_build,
		.event = cli_turn_details_event,
		.appear = cli_turn_details_appear,
		.disappear = cli_turn_details_disappear,
		.destroy = cli_turn_details_close};
	const struct morph_tui_view_controller_spec spec = {
		.ops = &ops, .data = ctx->details, .tick_ms = 250,
		.terminal_flags =
			MORPH_TUI_TERMINAL_SESSION_MOUSE | MORPH_TUI_TERMINAL_SESSION_FOCUS};
	if (!morph_tui_view_controller_present_borrow(app, &spec))
		ctx->details->open = 1;
}

void cli_turn_details_cleanup(struct cli_application *ctx)
{
	if (!ctx || !ctx->details)
		return;
	free(ctx->details->rendered);
	free(ctx->details);
	ctx->details = NULL;
}
