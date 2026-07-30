#include "runtime/mcp.h"

#include "util/buf.h"
#include "util/error.h"
#include "util/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *runtime_mcp_transport_name(
	enum mcp_transport_type transport)
{
	return transport == MCP_TRANSPORT_STDIO ? "stdio" : "http";
}

static const char *runtime_mcp_error_text(
	const struct mcp_server_config *server, int error_code,
	char *buf, size_t buf_cap)
{
	const char *token;

	if (!server || !buf || buf_cap == 0)
		return morph_strerror(error_code);
	if (server->transport != MCP_TRANSPORT_STREAMABLE_HTTP ||
	    error_code != MORPH_ERR_NOT_CONFIGURED)
		return morph_strerror(error_code);
	if (!server->http_auth_token_env[0]) {
		snprintf(buf, buf_cap, "MCP authentication token is not configured");
		return buf;
	}
	token = getenv(server->http_auth_token_env);
	if (!token || !token[0]) {
		snprintf(buf, buf_cap,
			 "Missing MCP token: environment variable '%s' is not set",
			 server->http_auth_token_env);
		return buf;
	}
	return morph_strerror(error_code);
}

static void runtime_mcp_emit(morph_event_cb cb, void *user_data,
			     const char *name, const char *phase,
			     const char *message,
			     const struct mcp_server_config *server,
			     int tools, int resources, int prompts,
			     int error_code)
{
	cJSON *data;
	morph_buf_t message_buf;
	const char *effective_message = message;
	const char *error_text;
	char error_buf[256];

	if (!cb || !server)
		return;
	error_text = runtime_mcp_error_text(server, error_code, error_buf,
					    sizeof(error_buf));
	memset(&message_buf, 0, sizeof(message_buf));
	if (morph_buf_init(&message_buf, 128) == 0) {
		if (strcmp(name, "mcp.connecting") == 0)
			(void)morph_buf_printf(&message_buf, "Connecting to %s",
					       server->name);
		else if (strcmp(name, "mcp.connected") == 0)
			(void)morph_buf_printf(&message_buf, "%s connected",
					       server->name);
		else if (strcmp(name, "mcp.discovering") == 0)
			(void)morph_buf_printf(&message_buf,
					       "Discovering %s capabilities",
					       server->name);
		else if (strcmp(name, "mcp.ready") == 0) {
			if (tools >= 0 || resources >= 0 || prompts >= 0)
				(void)morph_buf_printf(
					&message_buf,
					"%s ready (%d tools, %d resources, "
					"%d prompts)",
					server->name, tools, resources, prompts);
			else
				(void)morph_buf_printf(&message_buf, "%s ready",
						       server->name);
		}
		else if (strcmp(name, "mcp.failed") == 0)
			(void)morph_buf_printf(&message_buf, "%s failed: %s",
					       server->name,
					       error_text);
		else if (strcmp(name, "mcp.disconnected") == 0)
			(void)morph_buf_printf(&message_buf, "%s disconnected",
					       server->name);
		if (message_buf.len > 0)
			effective_message = morph_buf_cstr(&message_buf);
	}
	data = cJSON_CreateObject();
	if (!data) {
		morph_buf_cleanup(&message_buf);
		return;
	}
	cJSON_AddStringToObject(data, "server", server->name);
	cJSON_AddStringToObject(data, "transport",
				runtime_mcp_transport_name(server->transport));
	cJSON_AddBoolToObject(data, "auto_connect",
			      server->auto_connect ? 1 : 0);
	if (server->connect_timeout > 0)
		cJSON_AddNumberToObject(data, "timeout_seconds",
					server->connect_timeout);
	if (tools >= 0)
		cJSON_AddNumberToObject(data, "tools", tools);
	if (resources >= 0)
		cJSON_AddNumberToObject(data, "resources", resources);
	if (prompts >= 0)
		cJSON_AddNumberToObject(data, "prompts", prompts);
	if (error_code < 0) {
		cJSON_AddNumberToObject(data, "error_code", error_code);
		cJSON_AddStringToObject(data, "error", error_text);
	}
	(void)morph_event_emit_simple(cb, user_data, MORPH_EVENT_MCP,
				     name, phase, effective_message, data);
	cJSON_Delete(data);
	morph_buf_cleanup(&message_buf);
}

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

int runtime_mcp_client_connect(struct mcp_client *client,
			       morph_event_cb event_cb,
			       void *event_user_data)
{
	int rc;

	if (!client)
		MORPH_RETURN(-EINVAL);
	runtime_mcp_emit(event_cb, event_user_data, "mcp.connecting",
			 "begin", "Connecting MCP server", &client->config,
			 -1, -1, -1, 0);
	rc = mcp_ensure_connected(client);
	if (rc != 0) {
		runtime_mcp_emit(event_cb, event_user_data, "mcp.failed",
				 "failed", "MCP server unavailable",
				 &client->config, -1, -1, -1, rc);
		return rc;
	}
	runtime_mcp_emit(event_cb, event_user_data, "mcp.connected",
			 "end", "MCP server connected", &client->config,
			 -1, -1, -1, 0);
	return 0;
}

int runtime_mcp_client_disconnect(struct mcp_client *client,
				  morph_event_cb event_cb,
				  void *event_user_data)
{
	if (!client)
		MORPH_RETURN(-EINVAL);
	mcp_disconnect(client);
	runtime_mcp_emit(event_cb, event_user_data, "mcp.disconnected",
			 "end", "MCP server disconnected", &client->config,
			 -1, -1, -1, 0);
	return 0;
}

int runtime_mcp_client_discover(struct mcp_client *client,
				struct tool_registry *tools,
				morph_event_cb event_cb,
				void *event_user_data,
				int *tools_count,
				int *resources_count,
				int *prompts_count)
{
	int t = -1;
	int r = -1;
	int p = -1;
	int rc;

	if (!client || !tools)
		MORPH_RETURN(-EINVAL);
	runtime_mcp_emit(event_cb, event_user_data, "mcp.discovering",
			 "begin", "Discovering MCP capabilities",
			 &client->config, -1, -1, -1, 0);
	t = mcp_register_server_tools(client, tools);
	r = mcp_register_server_resources(client, tools);
	p = mcp_register_server_prompts(client, tools);
	if (tools_count)
		*tools_count = t;
	if (resources_count)
		*resources_count = r;
	if (prompts_count)
		*prompts_count = p;
	rc = t < 0 ? t : (r < 0 ? r : (p < 0 ? p : 0));
	if (rc < 0) {
		runtime_mcp_emit(event_cb, event_user_data, "mcp.failed",
				 "failed", "MCP discovery failed",
				 &client->config, t, r, p, rc);
		return rc;
	}
	runtime_mcp_emit(event_cb, event_user_data, "mcp.ready",
			 "ready", "MCP server ready", &client->config,
			 t, r, p, 0);
	return 0;
}

int runtime_mcp_init_from_config(struct mcp_registry *registry,
				 const struct config_mcp *config,
				 struct tool_registry *tools,
				 int connect_configured,
				 morph_event_cb event_cb,
				 void *event_user_data)
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
		rc = runtime_mcp_client_connect(client, event_cb,
						event_user_data);
		if (rc != 0) {
			log_warn("runtime: mcp connect failed for '%s': %s",
				 server.name, morph_strerror(rc));
			continue;
		}
		if (tools) {
			rc = runtime_mcp_client_discover(
				client, tools, event_cb, event_user_data,
				NULL, NULL, NULL);
			if (rc != 0)
				log_warn("runtime: mcp discovery failed for "
					 "'%s': %s", server.name,
					 morph_strerror(rc));
		} else {
			runtime_mcp_emit(event_cb, event_user_data, "mcp.ready",
					 "ready", "MCP server ready", &server,
					 -1, -1, -1, 0);
		}
		log_info("runtime: mcp auto-connected '%s'", server.name);
	}
	return 0;
}
