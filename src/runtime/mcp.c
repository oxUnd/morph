#include "runtime/mcp.h"

#include "util/error.h"
#include "util/log.h"

#include <errno.h>
#include <string.h>

int runtime_mcp_server_config_from_config(const struct config_mcp_server *config,
					  struct mcp_server_config *out)
{
	if (!config || !out)
		MORPH_RETURN(-EINVAL);
	memset(out, 0, sizeof(*out));
	strncpy(out->name, config->name, MCP_NAME_MAX - 1);
	out->transport = strcmp(config->transport, "http") == 0
		? MCP_TRANSPORT_STREAMABLE_HTTP
		: MCP_TRANSPORT_STDIO;
	if (out->transport == MCP_TRANSPORT_STDIO) {
		strncpy(out->command, config->command,
			sizeof(out->command) - 1);
		out->cmd_args_count = config->args_count;
		if (out->cmd_args_count > MCP_CMD_MAX)
			out->cmd_args_count = MCP_CMD_MAX;
		for (int i = 0; i < out->cmd_args_count; i++)
			strncpy(out->cmd_args[i], config->args[i],
				MCP_CMD_ARG_MAX - 1);
		out->env_count = config->env_count;
		if (out->env_count > MCP_ENV_MAX)
			out->env_count = MCP_ENV_MAX;
		for (int i = 0; i < out->env_count; i++) {
			strncpy(out->env_keys[i], config->env_keys[i],
				sizeof(out->env_keys[i]) - 1);
			strncpy(out->env_vals[i], config->env_vals[i],
				MCP_ENV_VAL_MAX - 1);
		}
	} else {
		strncpy(out->http_url, config->http_url,
			sizeof(out->http_url) - 1);
		strncpy(out->http_auth_token_env,
			config->http_auth_token_env,
			sizeof(out->http_auth_token_env) - 1);
	}
	out->auto_connect = config->auto_connect;
	out->connect_timeout = config->connect_timeout;
	return 0;
}

int runtime_mcp_register_configured(struct mcp_registry *registry,
				    const struct config_mcp *config)
{
	int rc;

	if (!registry || !config)
		MORPH_RETURN(-EINVAL);
	mcp_registry_init(registry);
	for (int i = 0; i < config->server_count; i++) {
		struct mcp_server_config server;
		rc = runtime_mcp_server_config_from_config(&config->servers[i],
							   &server);
		if (rc != 0)
			return rc;
		rc = mcp_registry_add(registry, &server);
		if (rc != 0)
			return rc;
	}
	return 0;
}

int runtime_mcp_init_from_config(struct mcp_registry *registry,
				 const struct config_mcp *config,
				 struct tool_registry *tools,
				 int connect_configured)
{
	int rc;

	if (!registry || !config)
		MORPH_RETURN(-EINVAL);
	mcp_registry_init(registry);
	for (int i = 0; i < config->server_count; i++) {
		struct mcp_server_config server;
		struct mcp_client *client;

		rc = runtime_mcp_server_config_from_config(&config->servers[i],
							   &server);
		if (rc != 0) {
			log_warn("runtime: mcp config skipped at index %d: %s",
				 i, morph_strerror(rc));
			continue;
		}
		rc = mcp_registry_add(registry, &server);
		if (rc != 0) {
			log_warn("runtime: mcp add failed for '%s': %s",
				 server.name, morph_strerror(rc));
			continue;
		}
		if (!connect_configured || !server.auto_connect)
			continue;
		client = mcp_registry_get(registry, server.name);
		if (!client)
			continue;
		rc = mcp_ensure_connected(client);
		if (rc != 0) {
			log_warn("runtime: mcp connect failed for '%s': %s",
				 server.name, morph_strerror(rc));
			continue;
		}
		if (tools) {
			mcp_register_server_tools(client, tools);
			mcp_register_server_resources(client, tools);
			mcp_register_server_prompts(client, tools);
		}
		log_info("runtime: mcp auto-connected '%s'", server.name);
	}
	return 0;
}
