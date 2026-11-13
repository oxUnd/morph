#include "sapi/cli/commands/mcp.h"
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include "mcp/mcp.h"
#include "config/config.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/mcp_tree.h"
#include "util/array.h"
#include "util/cleanup.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/commands/registry.h"

struct cli_mcp_descriptors {
	void *items;
};

static void cli_mcp_descriptors_cleanup(struct cli_mcp_descriptors *descriptors)
{
	runtime_mcp_list_free(descriptors->items);
}

static int cli_mcp_event_tree_visible(const struct cli_application *ctx)
{
	return ctx && ctx->presentation_ready && ctx->presentation_mode == CLI_PRESENT_INTERACTIVE;
}

static int cli_mcp_command_mcp(struct cli_application *ctx, int argc, char **argv)
{
	const char *sub = cli_cmd_arg_borrow(argc, argv, 1);

	if (!sub || strcmp(sub, "list") == 0 || strcmp(sub, "status") == 0) {
		int count = runtime_mcp_count(ctx->runtime);
		if (count < 0)
			return count;
		morph_array_t servers MORPH_CLEANUP(morph_array_cleanup) = {0};
		struct runtime_mcp_status server;
		int rc = morph_array_init(&servers, count ? (size_t)count : 1, sizeof(server));
		for (int i = 0; !rc && i < count; i++) {
			struct runtime_mcp_status *slot = morph_array_push_borrow(&servers);
			if (!slot)
				return -ENOMEM;
			rc = runtime_mcp_info(ctx->runtime, i, slot);
		}
		if (!rc)
			rc = cli_mcp_tree_write(ctx, servers.elts, servers.nelts);
		if (rc)
			CMD_ERROR(ctx, "failed to inspect MCP servers: %s", morph_strerror(rc));
		return rc;
	}

	if (!strcmp(sub, "tools") || !strcmp(sub, "resources") || !strcmp(sub, "prompts")) {
		const char *name = cli_cmd_arg_borrow(argc, argv, 2);
		if (!name) {
			CMD_ERROR(ctx, "usage: /mcp %s <server_name>", sub);
			return -EINVAL;
		}
		struct cli_mcp_descriptors descriptors
			MORPH_CLEANUP(cli_mcp_descriptors_cleanup) = {0};
		struct mcp_tool_desc *tools = NULL;
		struct mcp_resource_desc *resources = NULL;
		struct mcp_prompt_desc *prompts = NULL;
		int count = 0;
		int rc;
		if (!strcmp(sub, "tools")) {
			rc = runtime_mcp_list_tools(ctx->runtime, name, &tools, &count);
			descriptors.items = tools;
		} else if (!strcmp(sub, "resources")) {
			rc = runtime_mcp_list_resources(ctx->runtime, name, &resources, &count);
			descriptors.items = resources;
		} else {
			rc = runtime_mcp_list_prompts(ctx->runtime, name, &prompts, &count);
			descriptors.items = prompts;
		}
		if (rc) {
			CMD_ERROR(ctx, "failed to list %s: %s", sub, morph_strerror(rc));
			return rc;
		}
		morph_array_t rows MORPH_CLEANUP(morph_array_cleanup) = {0};
		struct cli_mcp_tree_item row;
		rc = morph_array_init(&rows, count ? (size_t)count : 1, sizeof(row));
		for (int i = 0; !rc && i < count; i++) {
			struct cli_mcp_tree_item *slot = morph_array_push_borrow(&rows);
			if (!slot)
				return -ENOMEM;
			if (tools)
				*slot = (struct cli_mcp_tree_item){tools[i].name,
					tools[i].description, NULL};
			else if (resources)
				*slot = (struct cli_mcp_tree_item){resources[i].name,
					resources[i].description, resources[i].uri};
			else
				*slot = (struct cli_mcp_tree_item){prompts[i].name,
					prompts[i].description, NULL};
		}
		return rc ? rc : cli_mcp_tree_items_write(ctx, sub, name, rows.elts, rows.nelts);
	}

	if (strcmp(sub, "connect") == 0) {
		const char *name = cli_cmd_arg_borrow(argc, argv, 2);
		if (!name) {
			CMD_ERROR(ctx, "usage: /mcp connect <server_name>");
			return -EINVAL;
		}
		struct runtime_mcp_status info;
		if (runtime_mcp_find(ctx->runtime, name, &info) != 0) {
			CMD_ERROR(ctx, "MCP server not found: %s", name);
			return -ENOENT;
		}
		if (info.state == RUNTIME_MCP_CONNECTING || info.state == RUNTIME_MCP_DISCOVERING ||
		    info.state == RUNTIME_MCP_REGISTERING) {
			CMD_OK(ctx, "MCP server '%s' is initializing", name);
			return 0;
		}
		if (info.state == RUNTIME_MCP_READY) {
			CMD_OK(ctx, "MCP server '%s' already connected", name);
			return 0;
		}
		int rc = runtime_mcp_connect(ctx->runtime, name);
		if (rc < 0) {
			if (!cli_mcp_event_tree_visible(ctx))
				CMD_ERROR(ctx, "failed to connect to '%s': %s", name,
					  morph_strerror(rc));
			return rc;
		}
		int tools;
		int resources;
		int prompts;
		rc = runtime_mcp_discover(ctx->runtime, name, &tools, &resources, &prompts);
		if (rc < 0) {
			if (!cli_mcp_event_tree_visible(ctx))
				CMD_ERROR(ctx, "failed to discover '%s': %s", name,
					  morph_strerror(rc));
			return rc;
		}
		if (!cli_mcp_event_tree_visible(ctx))
			CMD_OK(ctx, "MCP server '%s' connected", name);
		return 0;
	}

	if (strcmp(sub, "disconnect") == 0) {
		const char *name = cli_cmd_arg_borrow(argc, argv, 2);
		if (!name) {
			CMD_ERROR(ctx, "usage: /mcp disconnect <server_name>");
			return -EINVAL;
		}
		struct runtime_mcp_status info;
		if (runtime_mcp_find(ctx->runtime, name, &info) != 0) {
			CMD_ERROR(ctx, "MCP server not found: %s", name);
			return -ENOENT;
		}
		int rc = runtime_mcp_disconnect(ctx->runtime, name);
		if (rc < 0) {
			CMD_ERROR(ctx, "failed to disconnect '%s': %s", name, morph_strerror(rc));
			return rc;
		}
		CMD_OK(ctx, "MCP server '%s' disconnected", name);
		return 0;
	}

	CMD_ERROR(ctx,
		  "unknown MCP subcommand: %s. Try: list, tools, resources, "
		  "prompts, connect, disconnect",
		  sub);
	return -EINVAL;
}

static const struct cli_command cli_mcp_entries[] = {
	{"/mcp", cli_mcp_command_mcp, "List or manage MCP servers", "/mcp list"},
};

struct cli_command_group cli_mcp_commands(void)
{
	return (struct cli_command_group){
		.entries = cli_mcp_entries,
		.count = (int)(sizeof(cli_mcp_entries) / sizeof(cli_mcp_entries[0])),
		.category = "MCP",
	};
}
