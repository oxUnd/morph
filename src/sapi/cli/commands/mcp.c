#include "sapi/cli/commands/registry.h"

static int cmd_mcp(struct cli_context *ctx, int argc, char **argv)
{
	const char *sub = cli_cmd_arg(argc, argv, 1);

	if (!sub || strcmp(sub, "list") == 0 || strcmp(sub, "status") == 0) {
		int server_count = runtime_mcp_count(ctx->runtime);
		CMD_HEADER("MCP servers (%d)", server_count);
		if (server_count == 0) {
			printf("  (none — add servers to config.toml under [mcp.servers])\n");
			return 0;
		}
		for (int i = 0; i < server_count; i++) {
			struct runtime_mcp_status info;
			struct runtime_mcp_status *mc = &info;
			if (runtime_mcp_info(ctx->runtime, i, &info) != 0)
				continue;
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
		struct mcp_tool_desc *tools = NULL;
		int count = 0;
		int rc = runtime_mcp_list_tools(ctx->runtime, name, &tools, &count);
		if (rc < 0) {
			CMD_ERROR("failed to list tools: %s", morph_strerror(rc));
			return rc;
		}
		CMD_HEADER("MCP tools for '%s' (%d)", name, count);
		for (int i = 0; i < count; i++) {
			printf("  %-30s %s\n", tools[i].name, tools[i].description);
		}
		runtime_mcp_list_free(tools);
		return 0;
	}

	if (strcmp(sub, "resources") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /mcp resources <server_name>");
			return -EINVAL;
		}
		struct mcp_resource_desc *res = NULL;
		int count = 0;
		int rc = runtime_mcp_list_resources(ctx->runtime, name, &res, &count);
		if (rc < 0) {
			CMD_ERROR("failed to list resources: %s", morph_strerror(rc));
			return rc;
		}
		CMD_HEADER("MCP resources for '%s' (%d)", name, count);
		for (int i = 0; i < count; i++) {
			printf("  %-30s %s\n", res[i].name, res[i].description);
			printf("    uri: %s\n", res[i].uri);
		}
		runtime_mcp_list_free(res);
		return 0;
	}

	if (strcmp(sub, "prompts") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /mcp prompts <server_name>");
			return -EINVAL;
		}
		struct mcp_prompt_desc *prompts = NULL;
		int count = 0;
		int rc = runtime_mcp_list_prompts(ctx->runtime, name,
						  &prompts, &count);
		if (rc < 0) {
			CMD_ERROR("failed to list prompts: %s", morph_strerror(rc));
			return rc;
		}
		CMD_HEADER("MCP prompts for '%s' (%d)", name, count);
		for (int i = 0; i < count; i++) {
			printf("  %-30s %s\n", prompts[i].name, prompts[i].description);
		}
		runtime_mcp_list_free(prompts);
		return 0;
	}

	if (strcmp(sub, "connect") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /mcp connect <server_name>");
			return -EINVAL;
		}
		struct runtime_mcp_status info;
		if (runtime_mcp_find(ctx->runtime, name, &info) != 0) {
			CMD_ERROR("MCP server not found: %s", name);
			return -ENOENT;
		}
		if (info.connected) {
			CMD_OK("MCP server '%s' already connected", name);
			return 0;
		}
		int rc = runtime_mcp_connect(ctx->runtime, name);
		if (rc < 0) {
			CMD_ERROR("failed to connect to '%s': %s", name, morph_strerror(rc));
			return rc;
		}
		int tools;
		int resources;
		int prompts;
		rc = runtime_mcp_discover(ctx->runtime, name, &tools,
					  &resources, &prompts);
		if (rc < 0) {
			CMD_ERROR("failed to discover '%s': %s", name,
				  morph_strerror(rc));
			return rc;
		}
		CMD_OK("MCP server '%s' connected", name);
		return 0;
	}

	if (strcmp(sub, "disconnect") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /mcp disconnect <server_name>");
			return -EINVAL;
		}
		struct runtime_mcp_status info;
		if (runtime_mcp_find(ctx->runtime, name, &info) != 0) {
			CMD_ERROR("MCP server not found: %s", name);
			return -ENOENT;
		}
		int rc = runtime_mcp_disconnect(ctx->runtime, name);
		if (rc < 0) {
			CMD_ERROR("failed to disconnect '%s': %s", name,
				  morph_strerror(rc));
			return rc;
		}
		CMD_OK("MCP server '%s' disconnected", name);
		return 0;
	}

	CMD_ERROR("unknown MCP subcommand: %s. Try: list, tools, resources, "
		  "prompts, connect, disconnect", sub);
	return -EINVAL;
}


static const struct cli_command mcp_commands[] = {
	{ "/mcp",     cmd_mcp,     "List or manage MCP servers",        "/mcp list" },
};

int cli_register_mcp_commands(void)
{
	return cli_command_register_many(mcp_commands,
					 (int)(sizeof(mcp_commands) /
					 sizeof(mcp_commands[0])),
					 "MCP");
}
