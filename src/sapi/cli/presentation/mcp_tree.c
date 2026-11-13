#include "sapi/cli/presentation/mcp_tree.h"
#include "nb/err.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/widgets/list.h"
#include "sapi/cli/pages/chat.h"
#include "runtime/runtime.h"
#include "nb/cleanup.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>
#include <morph/tui/widgets/tree.h>
#include <errno.h>
#include <string.h>

static int cli_mcp_tree_line(nb_buf *out, const char *text, const char *prefix,
			     const char *continuation, const char *style, int columns)
{
	char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		text ? text : "", text ? strlen(text) : 0, MORPH_TERMINAL_TEXT_MULTILINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;
	for (char *p = safe; *p; p++)
		if (*p == '\t')
			*p = ' ';
	const char *cursor = safe;
	const char *newline = strchr(cursor, '\n');
	int first = 1;
	do {
		const char *branch = first ? prefix : continuation;
		int available = columns - (int)morph_terminal_display_width_ansi(branch);
		if (available < 1) {
			int rc = (nb_buf_append_cstr(out, "…\n") ? 0 : NB_ERR_NOMEM);
			return rc;
		}
		const char *end =
			morph_terminal_advance_display_width_borrow(cursor, (size_t)available);
		if (newline && newline < end)
			end = newline;
		int rc = (nb_buf_append_cstr(out, branch) ? 0 : NB_ERR_NOMEM);
		if (!rc)
			rc = (nb_buf_append_cstr(out, style) ? 0 : NB_ERR_NOMEM);
		if (end == cursor && *cursor && *cursor != '\n') {
			if (!rc)
				rc = (nb_buf_append_cstr(out, "…") ? 0 : NB_ERR_NOMEM);
			end = nb_utf8_advance(cursor, 1);
		} else if (!rc)
			rc = (nb_buf_append(out, cursor, (size_t)(end - cursor)) ? 0
										 : NB_ERR_NOMEM);
		if (!rc && *style)
			rc = (nb_buf_append_cstr(out, ANSI_RESET) ? 0 : NB_ERR_NOMEM);
		if (!rc)
			rc = (nb_buf_append_char(out, '\n') ? 0 : NB_ERR_NOMEM);
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

static int cli_mcp_tree_child(nb_buf *out, const int *ancestors, size_t depth, int last,
			      const char *text, int columns, const char *style)
{
	nb_buf prefix NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf continuation NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = morph_tui_widget_tree_prefix(&prefix, ancestors, depth, last, 0, 0);
	if (!rc)
		rc = morph_tui_widget_tree_prefix(&continuation, ancestors, depth, last, 1, 0);
	if (!rc)
		rc = cli_mcp_tree_line(out, text, prefix.ptr, continuation.ptr, style, columns);
	return rc;
}

static const char *cli_mcp_tree_state(const struct runtime_mcp_status *server)
{
	if (server->error_code == NB_ERR_CANCELLED || server->error_code == NB_ERR_INTERRUPTED)
		return "cancelled";
	if (server->error_code == NB_ERR_TIMEOUT)
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
	if (server->error_code == NB_ERR_CANCELLED || server->error_code == NB_ERR_INTERRUPTED)
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

static int cli_mcp_tree_server(nb_buf *out, const struct runtime_mcp_status *server, int columns,
			       int styled)
{
	nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf marker NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_printf(&title, "%s [%s] · %s", server->config.name,
				server->config.transport == MCP_TRANSPORT_STDIO ? "stdio" : "http",
				cli_mcp_tree_state(server))
			  ? 0
			  : NB_ERR_NOMEM);
	if (!rc)
		rc = (nb_buf_printf(&marker, "%s ", cli_mcp_tree_marker(server, styled))
			      ? 0
			      : NB_ERR_NOMEM);
	if (!rc)
		rc = cli_mcp_tree_line(out, title.ptr, marker.ptr, "│ ", styled ? ANSI_BOLD : "",
				       columns);
	if (rc)
		return rc;
	int fields = 1 + (server->elapsed_ms > 0) + (server->error_code < 0);
	if (server->connected)
		fields += 3;
	nb_buf field NB_CLEANUP(nb_buf_cleanup) = {0};
	rc = (nb_buf_printf(&field, "Transport: %s",
			    server->config.transport == MCP_TRANSPORT_STDIO ? "stdio" : "http")
		      ? 0
		      : NB_ERR_NOMEM);
	if (!rc)
		rc = cli_mcp_tree_child(out, NULL, 0, --fields == 0, field.ptr, columns, "");
	if (!rc && server->connected) {
		nb_buf_clear(&field);
		rc = (nb_buf_printf(&field, "Server: %s v%s", server->server_name,
				    server->server_version)
			      ? 0
			      : NB_ERR_NOMEM);
		if (!rc)
			rc = cli_mcp_tree_child(out, NULL, 0, --fields == 0, field.ptr, columns,
						"");
		nb_buf_clear(&field);
		if (!rc)
			rc = (nb_buf_printf(&field, "Protocol: %s", server->negotiated_version)
				      ? 0
				      : NB_ERR_NOMEM);
		if (!rc)
			rc = cli_mcp_tree_child(out, NULL, 0, --fields == 0, field.ptr, columns,
						"");
		int last = --fields == 0;
		if (!rc)
			rc = cli_mcp_tree_child(out, NULL, 0, last, "Capabilities", columns,
				styled ? ANSI_BOLD : "");
		const char *labels[] = {"Tools", "Resources", "Prompts"};
		int supported[] = {server->supports_tools, server->supports_resources,
			server->supports_prompts};
		for (size_t i = 0; !rc && i < 3; i++) {
			nb_buf_clear(&field);
			rc = (nb_buf_printf(&field, "%s: %s", labels[i],
					    supported[i] ? "yes" : "no")
				      ? 0
				      : NB_ERR_NOMEM);
			if (!rc)
				rc = cli_mcp_tree_child(out, &last, 1, i == 2, field.ptr, columns,
							"");
		}
	}
	if (!rc && server->elapsed_ms > 0) {
		nb_buf_clear(&field);
		rc = (nb_buf_printf(&field, "Elapsed: %lld ms", (long long)server->elapsed_ms)
			      ? 0
			      : NB_ERR_NOMEM);
		if (!rc)
			rc = cli_mcp_tree_child(out, NULL, 0, --fields == 0, field.ptr, columns,
						styled ? ANSI_DIM : "");
	}
	if (!rc && server->error_code < 0) {
		nb_buf_clear(&field);
		rc = (nb_buf_printf(&field, "Error: %s",
				    nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
					       server->error_code))
			      ? 0
			      : NB_ERR_NOMEM);
		if (!rc)
			rc = cli_mcp_tree_child(out, NULL, 0, 1, field.ptr, columns,
						styled && server->error_code != NB_ERR_CANCELLED &&
								server->error_code !=
									NB_ERR_INTERRUPTED
							? CLI_FEED_FAILED_STYLE
						: styled ? ANSI_DIM
							 : "");
	}
	return rc;
}

int cli_mcp_tree_render(nb_buf *out, const struct runtime_mcp_status *servers, size_t count,
			int columns, int styled)
{
	if (!out || (!servers && count) || columns < 1)
		return NB_ERR_INVAL;
	nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_printf(&title, "MCP servers (%zu)", count) ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = cli_mcp_tree_line(out, title.ptr, "", "", styled ? ANSI_BOLD : "", columns);
	if (!rc && !count)
		rc = cli_mcp_tree_child(out, NULL, 0, 1,
			"(none — add servers to config.toml under [mcp.servers])", columns, "");
	for (size_t i = 0; !rc && i < count; i++)
		rc = cli_mcp_tree_server(out, &servers[i], columns, styled);
	return rc;
}

int cli_mcp_tree_items_render(nb_buf *out, const char *kind, const char *server,
			      const struct cli_mcp_tree_item *items, size_t count, int columns,
			      int styled)
{
	if (!out || !kind || !server || (!items && count) || columns < 1)
		return NB_ERR_INVAL;
	nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_printf(&title, "MCP %s for '%s' (%zu)", kind, server, count)
			  ? 0
			  : NB_ERR_NOMEM);
	if (!rc)
		rc = cli_mcp_tree_line(out, title.ptr, "", "", styled ? ANSI_BOLD : "", columns);
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
			nb_buf_clear(&title);
			rc = (nb_buf_printf(&title, "URI: %s", items[i].uri) ? 0 : NB_ERR_NOMEM);
			if (!rc)
				rc = cli_mcp_tree_child(out, &last, 1, 1, title.ptr, columns,
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
	nb_buf rendered NB_CLEANUP(nb_buf_cleanup) = {0};
	int columns = cli_list_columns() - (int)CLI_CONTENT_RIGHT_PADDING;
	int rc = cli_mcp_tree_render(&rendered, servers, count, columns > 0 ? columns : 1,
		cli_output_color_enabled(ctx));
	if (!rc && cli_output_printf(ctx, "%s", rendered.ptr) < 0)
		rc = NB_ERR_IO;
	return rc;
}

int cli_mcp_tree_items_write(struct cli_application *ctx, const char *kind, const char *server,
	const struct cli_mcp_tree_item *items, size_t count)
{
	if (cli_chat_active(ctx))
		return cli_chat_mcp_items(ctx, kind, server, items, count);
	nb_buf rendered NB_CLEANUP(nb_buf_cleanup) = {0};
	int columns = cli_list_columns() - (int)CLI_CONTENT_RIGHT_PADDING;
	int rc = cli_mcp_tree_items_render(&rendered, kind, server, items, count,
		columns > 0 ? columns : 1, cli_output_color_enabled(ctx));
	if (!rc && cli_output_printf(ctx, "%s", rendered.ptr) < 0)
		rc = NB_ERR_IO;
	return rc;
}
