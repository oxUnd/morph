#include "cli/commands/registry.h"

static int cmd_mcp(struct cli_context *ctx, int argc, char **argv)
{
	const char *sub = cli_cmd_arg(argc, argv, 1);

	if (!sub || strcmp(sub, "list") == 0 || strcmp(sub, "status") == 0) {
		CMD_HEADER("MCP servers (%d)", ctx->mcp.count);
		if (ctx->mcp.count == 0) {
			printf("  (none — add servers to config.toml under [mcp.servers])\n");
			return 0;
		}
		for (int i = 0; i < ctx->mcp.count; i++) {
			struct mcp_client *mc = ctx->mcp.servers[i];
			const char *status = mc->connected ?
				ANSI_GREEN "connected" ANSI_RESET :
				ANSI_YELLOW "disconnected" ANSI_RESET;
			const char *transport = mc->config.transport ==
				MCP_TRANSPORT_STDIO ? "stdio" : "http";

			printf("  %s%-20s%s  [%s]  %s\n",
				ANSI_BOLD, mc->config.name, ANSI_RESET,
				transport, status);
			if (mc->connected) {
				printf("    server: %s v%s | proto: %s\n",
					mc->server_name, mc->server_version,
					mc->negotiated_version);
				printf("    tools: %-3s  resources: %-3s  prompts: %-3s\n",
					mc->supports_tools ? "yes" : "no",
					mc->supports_resources ? "yes" : "no",
					mc->supports_prompts ? "yes" : "no");
			}
		}
		return 0;
	}

	if (strcmp(sub, "tools") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /mcp tools <server_name>");
			return -EINVAL;
		}
		struct mcp_client *mc = mcp_registry_get(&ctx->mcp, name);
		if (!mc) {
			CMD_ERROR("MCP server not found: %s", name);
			return -ENOENT;
		}
		int rc = mcp_ensure_connected(mc);
		if (rc < 0) {
			CMD_ERROR("failed to connect to '%s': %s", name, morph_strerror(rc));
			return rc;
		}
		struct mcp_tool_desc *tools = NULL;
		int count = 0;
		struct arena *arena = arena_create(0);
		if (!arena) {
			CMD_ERROR("failed to create arena");
			return -ENOMEM;
		}
		rc = mcp_list_tools(mc, arena, &tools, &count);
		if (rc < 0) {
			CMD_ERROR("failed to list tools: %s", morph_strerror(rc));
			arena_destroy(arena);
			return rc;
		}
		CMD_HEADER("MCP tools for '%s' (%d)", name, count);
		for (int i = 0; i < count; i++) {
			printf("  %-30s %s\n", tools[i].name, tools[i].description);
		}
		arena_destroy(arena);
		return 0;
	}

	if (strcmp(sub, "resources") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /mcp resources <server_name>");
			return -EINVAL;
		}
		struct mcp_client *mc = mcp_registry_get(&ctx->mcp, name);
		if (!mc) {
			CMD_ERROR("MCP server not found: %s", name);
			return -ENOENT;
		}
		int rc = mcp_ensure_connected(mc);
		if (rc < 0) {
			CMD_ERROR("failed to connect to '%s': %s", name, morph_strerror(rc));
			return rc;
		}
		struct mcp_resource_desc *res = NULL;
		int count = 0;
		struct arena *arena = arena_create(0);
		if (!arena) {
			CMD_ERROR("failed to create arena");
			return -ENOMEM;
		}
		rc = mcp_list_resources(mc, arena, &res, &count);
		if (rc < 0) {
			CMD_ERROR("failed to list resources: %s", morph_strerror(rc));
			arena_destroy(arena);
			return rc;
		}
		CMD_HEADER("MCP resources for '%s' (%d)", name, count);
		for (int i = 0; i < count; i++) {
			printf("  %-30s %s\n", res[i].name, res[i].description);
			printf("    uri: %s\n", res[i].uri);
		}
		arena_destroy(arena);
		return 0;
	}

	if (strcmp(sub, "prompts") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /mcp prompts <server_name>");
			return -EINVAL;
		}
		struct mcp_client *mc = mcp_registry_get(&ctx->mcp, name);
		if (!mc) {
			CMD_ERROR("MCP server not found: %s", name);
			return -ENOENT;
		}
		int rc = mcp_ensure_connected(mc);
		if (rc < 0) {
			CMD_ERROR("failed to connect to '%s': %s", name, morph_strerror(rc));
			return rc;
		}
		struct mcp_prompt_desc *prompts = NULL;
		int count = 0;
		struct arena *arena = arena_create(0);
		if (!arena) {
			CMD_ERROR("failed to create arena");
			return -ENOMEM;
		}
		rc = mcp_list_prompts(mc, arena, &prompts, &count);
		if (rc < 0) {
			CMD_ERROR("failed to list prompts: %s", morph_strerror(rc));
			arena_destroy(arena);
			return rc;
		}
		CMD_HEADER("MCP prompts for '%s' (%d)", name, count);
		for (int i = 0; i < count; i++) {
			printf("  %-30s %s\n", prompts[i].name, prompts[i].description);
		}
		arena_destroy(arena);
		return 0;
	}

	if (strcmp(sub, "connect") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /mcp connect <server_name>");
			return -EINVAL;
		}
		struct mcp_client *mc = mcp_registry_get(&ctx->mcp, name);
		if (!mc) {
			CMD_ERROR("MCP server not found: %s", name);
			return -ENOENT;
		}
		if (mc->connected) {
			CMD_OK("MCP server '%s' already connected", name);
			return 0;
		}
		cli_emit_mcp_event(ctx, "mcp.connecting", "begin",
				   "manual MCP connect started", name,
				   mc->config.transport, 0,
				   mc->config.connect_timeout,
				   -1, -1, -1, 0);
		int rc = mcp_ensure_connected(mc);
		if (rc < 0) {
			cli_emit_mcp_event(ctx, "mcp.failed", "failed",
					   "manual MCP connect failed", name,
					   mc->config.transport, 0,
					   mc->config.connect_timeout,
					   -1, -1, -1, rc);
			CMD_ERROR("failed to connect to '%s': %s", name, morph_strerror(rc));
			return rc;
		}
		cli_emit_mcp_event(ctx, "mcp.connected", "end",
				   "manual MCP connect completed", name,
				   mc->config.transport, 0,
				   mc->config.connect_timeout,
				   -1, -1, -1, 0);
		cli_discover_mcp_server(ctx, mc, 0,
					mc->config.connect_timeout);
		CMD_OK("MCP server '%s' connected", name);
		return 0;
	}

	if (strcmp(sub, "disconnect") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /mcp disconnect <server_name>");
			return -EINVAL;
		}
		struct mcp_client *mc = mcp_registry_get(&ctx->mcp, name);
		if (!mc) {
			CMD_ERROR("MCP server not found: %s", name);
			return -ENOENT;
		}
		mcp_disconnect(mc);
		cli_emit_mcp_event(ctx, "mcp.disconnected", "end",
				   "MCP server disconnected", name,
				   mc->config.transport, 0,
				   mc->config.connect_timeout,
				   -1, -1, -1, 0);
		CMD_OK("MCP server '%s' disconnected", name);
		return 0;
	}

	CMD_ERROR("unknown MCP subcommand: %s. Try: list, tools, resources, prompts, connect, disconnect", sub);
	return -EINVAL;
}


static const struct cli_command mcp_commands[] = {
	{ "/mcp",     cmd_mcp,     "List or manage MCP servers",        "/mcp list" },
};

int cli_register_mcp_commands(void)
{
	return cli_command_register_many(mcp_commands,
		(int)(sizeof(mcp_commands) / sizeof(mcp_commands[0])));
}
