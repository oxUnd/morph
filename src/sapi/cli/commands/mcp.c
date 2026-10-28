#include "sapi/cli/commands/registry.h"

static int mcp_event_tree_visible(const struct cli_context *ctx)
{
	return ctx && ctx->presentation_ready &&
		ctx->presentation_mode == CLI_PRESENT_INTERACTIVE;
}

static int cmd_mcp(struct cli_context *ctx, int argc, char **argv)
{
	const char *sub = cli_cmd_arg(argc, argv, 1);

	if (!sub || strcmp(sub, "list") == 0 || strcmp(sub, "status") == 0) {
		int server_count = runtime_mcp_count(ctx->runtime);
		CMD_HEADER(ctx, "MCP servers (%d)", server_count);
		if (server_count == 0) {
			printf(ctx, "  (none — add servers to config.toml under [mcp.servers])\n");
			return 0;
		}
		for (int i = 0; i < server_count; i++) {
			struct runtime_mcp_status info;
			struct runtime_mcp_status *mc = &info;
			if (runtime_mcp_info(ctx->runtime, i, &info) != 0)
				continue;
			const char *states[] = {
				"disconnected", "connecting", "discovering",
				"registering", "ready", "failed"
			};
			const char *status = states[mc->state];
			const char *transport = mc->config.transport ==
				MCP_TRANSPORT_STDIO ? "stdio" : "http";

			printf(ctx, "  %s%-20s%s  [%s]  %s\n", ANSI_BOLD, mc->config.name,
			       ANSI_RESET, transport, status);
			if (mc->elapsed_ms > 0)
				printf(ctx, "    elapsed: %lld ms\n", (long long)mc->elapsed_ms);
			if (mc->error_code < 0)
				printf(ctx, "    error: %s\n", morph_strerror(mc->error_code));
			if (mc->connected) {
				printf(ctx, "    server: %s v%s | proto: %s\n", mc->server_name,
				       mc->server_version, mc->negotiated_version);
				printf(ctx, "    tools: %-3s  resources: %-3s  prompts: %-3s\n",
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
			CMD_ERROR(ctx, "usage: /mcp tools <server_name>");
			return -EINVAL;
		}
		struct mcp_tool_desc *tools = NULL;
		int count = 0;
		int rc = runtime_mcp_list_tools(ctx->runtime, name, &tools, &count);
		if (rc < 0) {
			CMD_ERROR(ctx, "failed to list tools: %s", morph_strerror(rc));
			return rc;
		}
		CMD_HEADER(ctx, "MCP tools for '%s' (%d)", name, count);
		for (int i = 0; i < count; i++) {
			printf(ctx, "  %-30s %s\n", tools[i].name, tools[i].description);
		}
		runtime_mcp_list_free(tools);
		return 0;
	}

	if (strcmp(sub, "resources") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR(ctx, "usage: /mcp resources <server_name>");
			return -EINVAL;
		}
		struct mcp_resource_desc *res = NULL;
		int count = 0;
		int rc = runtime_mcp_list_resources(ctx->runtime, name, &res, &count);
		if (rc < 0) {
			CMD_ERROR(ctx, "failed to list resources: %s", morph_strerror(rc));
			return rc;
		}
		CMD_HEADER(ctx, "MCP resources for '%s' (%d)", name, count);
		for (int i = 0; i < count; i++) {
			printf(ctx, "  %-30s %s\n", res[i].name, res[i].description);
			printf(ctx, "    uri: %s\n", res[i].uri);
		}
		runtime_mcp_list_free(res);
		return 0;
	}

	if (strcmp(sub, "prompts") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR(ctx, "usage: /mcp prompts <server_name>");
			return -EINVAL;
		}
		struct mcp_prompt_desc *prompts = NULL;
		int count = 0;
		int rc = runtime_mcp_list_prompts(ctx->runtime, name,
						  &prompts, &count);
		if (rc < 0) {
			CMD_ERROR(ctx, "failed to list prompts: %s", morph_strerror(rc));
			return rc;
		}
		CMD_HEADER(ctx, "MCP prompts for '%s' (%d)", name, count);
		for (int i = 0; i < count; i++) {
			printf(ctx, "  %-30s %s\n", prompts[i].name, prompts[i].description);
		}
		runtime_mcp_list_free(prompts);
		return 0;
	}

	if (strcmp(sub, "connect") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR(ctx, "usage: /mcp connect <server_name>");
			return -EINVAL;
		}
		struct runtime_mcp_status info;
		if (runtime_mcp_find(ctx->runtime, name, &info) != 0) {
			CMD_ERROR(ctx, "MCP server not found: %s", name);
			return -ENOENT;
		}
		if (info.state == RUNTIME_MCP_CONNECTING ||
		    info.state == RUNTIME_MCP_DISCOVERING ||
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
			if (!mcp_event_tree_visible(ctx))
				CMD_ERROR(ctx, "failed to connect to '%s': %s", name,
					  morph_strerror(rc));
			return rc;
		}
		int tools;
		int resources;
		int prompts;
		rc = runtime_mcp_discover(ctx->runtime, name, &tools,
					  &resources, &prompts);
		if (rc < 0) {
			if (!mcp_event_tree_visible(ctx))
				CMD_ERROR(ctx, "failed to discover '%s': %s", name,
					  morph_strerror(rc));
			return rc;
		}
		if (!mcp_event_tree_visible(ctx))
			CMD_OK(ctx, "MCP server '%s' connected", name);
		return 0;
	}

	if (strcmp(sub, "disconnect") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
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


static const struct cli_command mcp_commands[] = {
	{ "/mcp",     cmd_mcp,     "List or manage MCP servers",        "/mcp list" },
};

struct cli_command_group cli_mcp_commands(void)
{
	return (struct cli_command_group){
		.entries = mcp_commands,
		.count = (int)(sizeof(mcp_commands) / sizeof(mcp_commands[0])),
		.category = "MCP",
	};
}
