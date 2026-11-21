#include "sapi/cli/pages/mcp_status.h"

#include "sapi/cli/application.h"
#include "sapi/cli/components/mcp_status.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/presentation/notice.h"
#include "sapi/cli/presentation/panel.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/services/event_queue.h"
#include "runtime/services.h"
#include "errors.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/time.h>
#include <nothing/tui/view_controller.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/scroll_view.h>
#include <nothing/tui/views/stack_view.h>
#include <nothing/tui/views/card_view.h>

#include <stdio.h>

struct cli_mcp_status_page {
	struct nt_view_controller base;
	struct nt_view *root, *statuses, *scroll, *empty;
	int64_t next_refresh;
};

static const char *cli_mcp_status_page_label(enum runtime_mcp_state state)
{
	switch (state) {
	case RUNTIME_MCP_CONNECTING:
		return "Connecting";
	case RUNTIME_MCP_DISCOVERING:
		return "Discovering capabilities";
	case RUNTIME_MCP_REGISTERING:
		return "Registering capabilities";
	case RUNTIME_MCP_READY:
		return "Ready";
	case RUNTIME_MCP_FAILED:
		return "Failed";
	case RUNTIME_MCP_DISCONNECTED:
		return "Disconnected";
	}

	return "Unknown";
}

static enum cli_notice_status cli_mcp_status_page_outcome(enum runtime_mcp_state state)
{
	switch (state) {
	case RUNTIME_MCP_CONNECTING:
	case RUNTIME_MCP_DISCOVERING:
	case RUNTIME_MCP_REGISTERING:
		return CLI_NOTICE_RUNNING;
	case RUNTIME_MCP_READY:
		return CLI_NOTICE_DONE;
	case RUNTIME_MCP_FAILED:
		return CLI_NOTICE_FAILED;
	case RUNTIME_MCP_DISCONNECTED:
		return CLI_NOTICE_PENDING;
	}

	return CLI_NOTICE_SYSTEM;
}

static int cli_mcp_status_page_refresh(struct cli_mcp_status_page *page)
{
	struct cli_application *ctx =
		cli_application_from_controller(nt_view_controller(page->root));
	int count = runtime_mcp_count(ctx->runtime);
	int rows = count;
	for (int i = 0; i < count; i++) {
		struct runtime_mcp_status status = {0};
		int rc = runtime_mcp_info(ctx->runtime, i, &status);
		if (rc)
			return rc;

		char title[BUFSIZ];
		const char *label = cli_mcp_status_page_label(status.state);
		snprintf(title, sizeof(title), "%s%s", label,
			status.error_code == NB_ERR_TIMEOUT ? " · TIMEOUT" : "");
		const char *error = status.state == RUNTIME_MCP_FAILED && status.error_code < 0
			? nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
				status.error_code) : NULL;
		const struct cli_notice notice = {
			.status = cli_mcp_status_page_outcome(status.state),
			.title = title,
			.detail = error,
		};
		rows += error != NULL;
		rc = cli_mcp_status_update(page->statuses, status.config.name, &notice);
		if (rc)
			return rc;
	}

	struct nt_view_rect content = nt_view_content_rect(page->scroll);
	if (content.width > 0) {
		struct nt_view_size size = {0};
		int rc = nt_view_measure(page->statuses, content.width, &size);
		if (rc)
			return rc;

		rows = size.height;
	}

	struct nt_view_options options = nt_view_options(page->scroll);
	int height = rows > 6 ? 6 : rows > 0 ? rows : 1;
	int rc = 0;
	if (options.height != height) {
		options.height = height;
		rc = nt_view_set_options(page->scroll, &options);
	}

	if (!rc)
		rc = nt_view_set_hidden(page->empty, count != 0);
	if (!rc)
		rc = nt_view_set_hidden(page->scroll, count == 0);

	page->next_refresh = nb_time_monotonic_ns() / 1000000 + 250;
	return rc;
}

static int cli_mcp_status_page_event(void *data, const struct nt_event *event)
{
	struct cli_mcp_status_page *page = data;
	if ((event->type != NT_EVENT_TICK && event->type != NT_EVENT_WAKE) ||
	    nb_time_monotonic_ns() / 1000000 < page->next_refresh)
		return 0;

	return cli_mcp_status_page_refresh(page);
}

static struct nt_view *cli_mcp_status_page_build(struct nt_builder *builder,
	struct nt_view *parent, void *data)
{
	struct cli_mcp_status_page *page = data;
	struct cli_application *ctx = cli_application_from_view(parent);
	int styled = cli_output_color_enabled(ctx);
	struct nt_text_style style = {0};
	if (styled)
		style.background = (struct nt_color){NT_COLOR_INDEXED, CLI_FLOAT_BACKGROUND_INDEX};

	struct nt_view *root = nt_card_view_add(builder, parent, "MCP connections",
		&(struct nt_view_options){.position = NT_VIEW_POSITION_ABSOLUTE,
			.anchor = NT_ANCHOR_BOTTOM_RIGHT, .x = -1, .width = 54,
			.max_width = NT_PERCENT(100), .max_height = NT_PERCENT(100),
			.style = style});
	if (!root)
		return NULL;

	page->root = root;
	struct nt_view *body = nt_column_add(builder, root, 0,
		&(struct nt_view_options){
			.padding = {.top = 1, .right = 1, .bottom = 1, .left = 1}});
	const struct nt_view_options hint = cli_panel_hint(1);
	page->scroll = nt_scroll_view_add(builder, body,
		&(struct nt_view_options){.max_height = 6});
	page->statuses = cli_mcp_status_add(builder, page->scroll,
		cli_output_color_enabled(ctx));
	page->empty = nt_label_add(builder, body, "No MCP servers configured.", NULL);
	nt_label_add(builder, body, "Esc close · click outside", &hint);
	if (nt_builder_error(builder))
		return NULL;

	struct nt_view_options statuses = nt_view_options(page->statuses);
	statuses.margin = (struct nt_view_insets){0};
	nt_builder_fail(builder, nt_view_set_options(page->statuses, &statuses));
	nt_builder_fail(builder, nt_view_set_focusable(page->scroll, 0));

	nt_builder_fail(builder, cli_mcp_status_page_refresh(page));
	return nt_builder_error(builder) ? NULL : root;
}

static void cli_mcp_status_page_complete(struct cli_application *ctx, void *data, int result)
{
	struct cli_mcp_status_page *page = data;
	(void)ctx;
	(void)result;
	nt_view_controller_destroy(&page->base);
}

int cli_mcp_status_page_show(struct cli_application *ctx)
{
	if (!ctx || !cli_chat_active(ctx))
		return NB_ERR_INVAL;

	if (!cli_event_queue_is_owner(ctx))
		return NB_ERR_NOT_PERMITTED;

	static const struct nt_view_controller_class type = {
		.superclass = &cli_application_panel_class,
		.instance_size = sizeof(struct cli_mcp_status_page),
	};
	struct nt_view_controller *owner NB_CLEANUP(cli_application_controller_cleanup) = NULL;
	int rc = nt_view_controller_create(&type, &owner);
	if (rc)
		return rc;

	struct cli_mcp_status_page *page = (struct cli_mcp_status_page *)owner;
	rc = cli_chat_overlay_mount(ctx, cli_mcp_status_page_build, page,
		cli_mcp_status_page_complete);
	if (!rc) {
		owner = NULL;
		cli_chat_overlay_set_refresh(ctx, cli_mcp_status_page_event);
	}

	return rc;
}
