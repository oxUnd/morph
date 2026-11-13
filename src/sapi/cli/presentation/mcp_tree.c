#include "sapi/cli/presentation/mcp_tree.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/widgets/list.h"
#include "sapi/cli/pages/chat.h"
#include "runtime/runtime.h"
#include "util/cleanup.h"
#include "util/utf8.h"
#include <morph/tui/widgets/tree.h>
#include <errno.h>
#include <string.h>

static int cli_mcp_tree_line(morph_buf_t *out, const char *text, const char *prefix,
	const char *continuation, const char *style, int columns)
{
	char *safe MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(
		text ? text : "", text ? strlen(text) : 0, UTF8_TERMINAL_TEXT_MULTILINE, NULL);
	if (!safe)
		return -ENOMEM;
	for (char *p = safe; *p; p++)
		if (*p == '\t')
			*p = ' ';
	const char *cursor = safe;
	const char *newline = strchr(cursor, '\n');
	int first = 1;
	do {
		const char *branch = first ? prefix : continuation;
		int available = columns - (int)utf8_display_width_ansi(branch);
		if (available < 1) {
			int rc = morph_buf_puts(out, "…\n");
			return rc;
		}
		const char *end = utf8_advance_display_width_borrow(cursor, (size_t)available);
		if (newline && newline < end)
			end = newline;
		int rc = morph_buf_puts(out, branch);
		if (!rc)
			rc = morph_buf_puts(out, style);
		if (end == cursor && *cursor && *cursor != '\n') {
			if (!rc)
				rc = morph_buf_puts(out, "…");
			end = utf8_advance_codepoints_borrow(cursor, 1);
		} else if (!rc)
			rc = morph_buf_append(out, cursor, (size_t)(end - cursor));
		if (!rc && *style)
			rc = morph_buf_puts(out, ANSI_RESET);
		if (!rc)
			rc = morph_buf_putc(out, '\n');
		if (rc)
			return rc;
		if (*end == '\n') {
			cursor = end + 1;
			newline = strchr(cursor, '\n');
		} else
			cursor = end;
		first = 0;
	} while (*cursor);
	return 0;
}

static int cli_mcp_tree_child(morph_buf_t *out, const int *ancestors, size_t depth,
	int last, const char *text, int columns, const char *style)
{
	morph_buf_t prefix MORPH_CLEANUP(morph_buf_cleanup) = {0};
	morph_buf_t continuation MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_tui_widget_tree_prefix(&prefix, ancestors, depth, last, 0, 0);
	if (!rc)
		rc = morph_tui_widget_tree_prefix(&continuation, ancestors, depth, last, 1, 0);
	if (!rc)
		rc = cli_mcp_tree_line(out, text, prefix.data, continuation.data, style, columns);
	return rc;
}

static const char *cli_mcp_tree_state(const struct runtime_mcp_status *server)
{
	if (server->error_code == -ECANCELED || server->error_code == -EINTR)
		return "cancelled";
	if (server->error_code == -ETIMEDOUT)
		return "failed · TIMEOUT";
	switch (server->state) {
	case RUNTIME_MCP_DISCONNECTED: return "disconnected";
	case RUNTIME_MCP_CONNECTING: return "connecting";
	case RUNTIME_MCP_DISCOVERING: return "discovering";
	case RUNTIME_MCP_REGISTERING: return "registering";
	case RUNTIME_MCP_READY: return "ready";
	case RUNTIME_MCP_FAILED: return "failed";
	}
	return "unknown";
}

static const char *cli_mcp_tree_marker(const struct runtime_mcp_status *server, int styled)
{
	if (server->error_code == -ECANCELED || server->error_code == -EINTR)
		return styled ? ANSI_DIM CLI_FEED_CANCELLED ANSI_RESET : CLI_FEED_CANCELLED;
	if (server->error_code < 0 || server->state == RUNTIME_MCP_FAILED)
		return styled ? CLI_FEED_FAILED_STYLE CLI_FEED_FAILED ANSI_RESET : CLI_FEED_FAILED;
	if (server->state == RUNTIME_MCP_READY)
		return styled ? ANSI_GREEN CLI_FEED_DONE ANSI_RESET : CLI_FEED_DONE;
	if (server->state == RUNTIME_MCP_CONNECTING || server->state == RUNTIME_MCP_DISCOVERING ||
	    server->state == RUNTIME_MCP_REGISTERING)
		return styled ? ANSI_CYAN CLI_FEED_RUNNING ANSI_RESET : CLI_FEED_RUNNING;
	return styled ? ANSI_DIM CLI_FEED_PENDING ANSI_RESET : CLI_FEED_PENDING;
}

static int cli_mcp_tree_server(morph_buf_t *out, const struct runtime_mcp_status *server,
	int columns, int styled)
{
	morph_buf_t title MORPH_CLEANUP(morph_buf_cleanup) = {0};
	morph_buf_t marker MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_buf_printf(&title, "%s [%s] · %s", server->config.name,
		server->config.transport == MCP_TRANSPORT_STDIO ? "stdio" : "http",
		cli_mcp_tree_state(server));
	if (!rc)
		rc = morph_buf_printf(&marker, "%s ", cli_mcp_tree_marker(server, styled));
	if (!rc)
		rc = cli_mcp_tree_line(out, title.data, marker.data, "│ ",
			styled ? ANSI_BOLD : "", columns);
	if (rc)
		return rc;
	int fields = 1 + (server->elapsed_ms > 0) + (server->error_code < 0);
	if (server->connected)
		fields += 3;
	morph_buf_t field MORPH_CLEANUP(morph_buf_cleanup) = {0};
	rc = morph_buf_printf(&field, "Transport: %s",
		server->config.transport == MCP_TRANSPORT_STDIO ? "stdio" : "http");
	if (!rc)
		rc = cli_mcp_tree_child(out, NULL, 0, --fields == 0, field.data, columns, "");
	if (!rc && server->connected) {
		morph_buf_reset(&field);
		rc = morph_buf_printf(&field, "Server: %s v%s", server->server_name,
			server->server_version);
		if (!rc)
			rc = cli_mcp_tree_child(out, NULL, 0, --fields == 0,
				field.data, columns, "");
		morph_buf_reset(&field);
		if (!rc)
			rc = morph_buf_printf(&field, "Protocol: %s", server->negotiated_version);
		if (!rc)
			rc = cli_mcp_tree_child(out, NULL, 0, --fields == 0,
				field.data, columns, "");
		int last = --fields == 0;
		if (!rc)
			rc = cli_mcp_tree_child(out, NULL, 0, last, "Capabilities", columns,
				styled ? ANSI_BOLD : "");
		const char *labels[] = {"Tools", "Resources", "Prompts"};
		int supported[] = {server->supports_tools, server->supports_resources,
			server->supports_prompts};
		for (size_t i = 0; !rc && i < 3; i++) {
			morph_buf_reset(&field);
			rc = morph_buf_printf(&field, "%s: %s", labels[i],
				supported[i] ? "yes" : "no");
			if (!rc)
				rc = cli_mcp_tree_child(out, &last, 1, i == 2,
					field.data, columns, "");
		}
	}
	if (!rc && server->elapsed_ms > 0) {
		morph_buf_reset(&field);
		rc = morph_buf_printf(&field, "Elapsed: %lld ms", (long long)server->elapsed_ms);
		if (!rc)
			rc = cli_mcp_tree_child(out, NULL, 0, --fields == 0, field.data, columns,
				styled ? ANSI_DIM : "");
	}
	if (!rc && server->error_code < 0) {
		morph_buf_reset(&field);
		rc = morph_buf_printf(&field, "Error: %s", morph_strerror(server->error_code));
		if (!rc)
			rc = cli_mcp_tree_child(out, NULL, 0, 1, field.data, columns,
				styled && server->error_code != -ECANCELED &&
					server->error_code != -EINTR
					? CLI_FEED_FAILED_STYLE : styled ? ANSI_DIM : "");
	}
	return rc;
}

int cli_mcp_tree_render(morph_buf_t *out, const struct runtime_mcp_status *servers,
	size_t count, int columns, int styled)
{
	if (!out || (!servers && count) || columns < 1)
		return -EINVAL;
	morph_buf_t title MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_buf_printf(&title, "MCP servers (%zu)", count);
	if (!rc)
		rc = cli_mcp_tree_line(out, title.data, "", "", styled ? ANSI_BOLD : "", columns);
	if (!rc && !count)
		rc = cli_mcp_tree_child(out, NULL, 0, 1,
			"(none — add servers to config.toml under [mcp.servers])", columns, "");
	for (size_t i = 0; !rc && i < count; i++)
		rc = cli_mcp_tree_server(out, &servers[i], columns, styled);
	return rc;
}

int cli_mcp_tree_items_render(morph_buf_t *out, const char *kind, const char *server,
	const struct cli_mcp_tree_item *items, size_t count, int columns, int styled)
{
	if (!out || !kind || !server || (!items && count) || columns < 1)
		return -EINVAL;
	morph_buf_t title MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_buf_printf(&title, "MCP %s for '%s' (%zu)", kind, server, count);
	if (!rc)
		rc = cli_mcp_tree_line(out, title.data, "", "", styled ? ANSI_BOLD : "", columns);
	if (!rc && !count)
		rc = cli_mcp_tree_child(out, NULL, 0, 1, "(none)", columns, "");
	for (size_t i = 0; !rc && i < count; i++) {
		int last = i + 1 == count;
		rc = cli_mcp_tree_child(out, NULL, 0, last, items[i].name, columns,
			styled ? ANSI_BOLD : "");
		int uri = items[i].uri && *items[i].uri;
		if (!rc && items[i].description && *items[i].description)
			rc = cli_mcp_tree_child(out, &last, 1, !uri,
				items[i].description, columns, "");
		if (!rc && uri) {
			morph_buf_reset(&title);
			rc = morph_buf_printf(&title, "URI: %s", items[i].uri);
			if (!rc)
				rc = cli_mcp_tree_child(out, &last, 1, 1, title.data, columns,
					styled ? ANSI_DIM : "");
		}
	}
	return rc;
}

int cli_mcp_tree_write(struct cli_application *ctx, const struct runtime_mcp_status *servers,
	size_t count)
{
	if (cli_chat_active(ctx))
		return cli_chat_mcp_servers(ctx, servers, count);
	morph_buf_t rendered MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int columns = cli_list_columns() - (int)CLI_CONTENT_RIGHT_PADDING;
	int rc = cli_mcp_tree_render(&rendered, servers, count, columns > 0 ? columns : 1,
		cli_output_color_enabled(ctx));
	if (!rc && cli_output_printf(ctx, "%s", rendered.data) < 0)
		rc = -EIO;
	return rc;
}

int cli_mcp_tree_items_write(struct cli_application *ctx, const char *kind, const char *server,
	const struct cli_mcp_tree_item *items, size_t count)
{
	if (cli_chat_active(ctx))
		return cli_chat_mcp_items(ctx, kind, server, items, count);
	morph_buf_t rendered MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int columns = cli_list_columns() - (int)CLI_CONTENT_RIGHT_PADDING;
	int rc = cli_mcp_tree_items_render(&rendered, kind, server, items, count,
		columns > 0 ? columns : 1, cli_output_color_enabled(ctx));
	if (!rc && cli_output_printf(ctx, "%s", rendered.data) < 0)
		rc = -EIO;
	return rc;
}
