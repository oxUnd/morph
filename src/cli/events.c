#include "cli/internal.h"

int cli_event_callback(const struct morph_event *ev, void *user_data)
{
	struct cli_context *ctx = user_data;
	const char *prefix = NULL;

	if (!ctx || !ev || ctx->event_mode == CLI_EVENTS_NONE)
		return 0;

	if (ctx->event_mode == CLI_EVENTS_JSON) {
		char *json = morph_event_to_json_string(ev);
		if (!json)
			return -ENOMEM;
		printf("%s\n", json);
		fflush(stdout);
		free(json);
		return 0;
	}

	if (ev->type == MORPH_EVENT_HITL &&
	    ev->name && strcmp(ev->name, "auth.required") == 0) {
		cJSON *data = ev->data;
		cJSON *backend = data ? cJSON_GetObjectItem(data, "backend") : NULL;
		cJSON *provider = data ? cJSON_GetObjectItem(data, "provider") : NULL;
		cJSON *model = data ? cJSON_GetObjectItem(data, "model") : NULL;
		cJSON *env_name = data ? cJSON_GetObjectItem(data, "env_name") : NULL;
		cJSON *tool = data ? cJSON_GetObjectItem(data, "tool") : NULL;
		const char *backend_s = cJSON_IsString(backend) ?
			backend->valuestring : "";
		const char *provider_s = cJSON_IsString(provider) ?
			provider->valuestring : "";
		const char *model_s = cJSON_IsString(model) ?
			model->valuestring : "";
		const char *env_s = cJSON_IsString(env_name) ?
			env_name->valuestring : "";
		const char *tool_s = cJSON_IsString(tool) ?
			tool->valuestring : "";

		if (!env_s[0]) {
			if (strcmp(backend_s, "image") == 0)
				env_s = ctx->config.models.image.api_key_env;
			else if (strcmp(backend_s, "video") == 0)
				env_s = ctx->config.models.video.api_key_env;
			else
				env_s = ctx->config.models.text.api_key_env;
		}
		printf(ANSI_BOLD ANSI_YELLOW "auth required: " ANSI_RESET);
		if (tool_s[0])
			printf("%s tool %s", backend_s, tool_s);
		else if (provider_s[0] || model_s[0])
			printf("%s model %s%s%s", backend_s,
			       provider_s, provider_s[0] && model_s[0] ? "/" : "",
			       model_s);
		else
			printf("%s model", backend_s[0] ? backend_s : "configured");
		printf(" is missing an API key.\n");
		if (env_s[0]) {
			printf("Edit %s or export %s, then retry.\n",
			       ctx->config_path[0] ? ctx->config_path :
			       default_config_path, env_s);
		} else {
			printf("Edit %s, then retry.\n",
			       ctx->config_path[0] ? ctx->config_path :
			       default_config_path);
		}
		fflush(stdout);
		return 0;
	}

	if (ev->type == MORPH_EVENT_TOOL && ev->name &&
	    strcmp(ev->name, "tool.stream.delta") == 0) {
		cJSON *data = ev->data;
		cJSON *kind = data ? cJSON_GetObjectItem(data, "kind") : NULL;
		cJSON *text = data ? cJSON_GetObjectItem(data, "text") : NULL;
		const char *kind_s = cJSON_IsString(kind) ?
			kind->valuestring : "text";
		const char *text_s = cJSON_IsString(text) ?
			text->valuestring : "";

		if (strcmp(kind_s, "text") == 0 && text_s[0]) {
			printf("%s", text_s);
			fflush(stdout);
		} else if (strcmp(kind_s, "status") == 0 && text_s[0]) {
			printf("\n[tool] %s\n", text_s);
			fflush(stdout);
		}
		return 0;
	}

	if (ev->type == MORPH_EVENT_STARTUP)
		prefix = "startup";
	else if (ev->type == MORPH_EVENT_MCP)
		prefix = "mcp";
	else if (ev->type == MORPH_EVENT_BACKGROUND)
		prefix = "background";
	else if (ev->type == MORPH_EVENT_TASK)
		prefix = "task";
	else if (ev->type == MORPH_EVENT_ARTIFACT)
		prefix = "artifact";

	if (prefix) {
		printf("[%s] %s\n", prefix,
		       ev->message ? ev->message : ev->name);
		fflush(stdout);
	}
	return 0;
}

static int cli_emit_event_simple(struct cli_context *ctx,
				 enum morph_event_type type,
				 const char *name, const char *phase,
				 const char *message, cJSON *data)
{
	if (!ctx)
		return -EINVAL;
	return morph_event_emit_simple(ctx->event_cb, ctx->event_user_data,
				       type, name, phase, message, data);
}

int cli_emit_startup_event(struct cli_context *ctx,
				  const char *name, const char *phase,
				  const char *message, const char *component,
				  int error_code)
{
	cJSON *data = cJSON_CreateObject();
	int rc;

	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "component", component ? component : "");
	if (error_code < 0) {
		cJSON_AddNumberToObject(data, "error_code", error_code);
		cJSON_AddStringToObject(data, "error",
					morph_strerror(error_code));
	}
	rc = cli_emit_event_simple(ctx, MORPH_EVENT_STARTUP, name, phase,
				   message, data);
	cJSON_Delete(data);
	return rc;
}

int cli_emit_background_event(struct cli_context *ctx,
				     const char *name, const char *phase,
				     const char *message, const char *task,
				     int count, int error_code)
{
	cJSON *data = cJSON_CreateObject();
	int rc;

	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "task", task ? task : "");
	if (count >= 0)
		cJSON_AddNumberToObject(data, "count", count);
	if (error_code < 0) {
		cJSON_AddNumberToObject(data, "error_code", error_code);
		cJSON_AddStringToObject(data, "error",
					morph_strerror(error_code));
	}
	rc = cli_emit_event_simple(ctx, MORPH_EVENT_BACKGROUND, name, phase,
				   message, data);
	cJSON_Delete(data);
	return rc;
}

static const char *mcp_transport_name(enum mcp_transport_type transport)
{
	return transport == MCP_TRANSPORT_STDIO ? "stdio" : "http";
}

int cli_emit_mcp_event(struct cli_context *ctx,
			      const char *name, const char *phase,
			      const char *message, const char *server,
			      enum mcp_transport_type transport,
			      int auto_connect, int timeout_seconds,
			      int tools, int resources, int prompts,
			      int error_code)
{
	cJSON *data = cJSON_CreateObject();
	int rc;

	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "server", server ? server : "");
	cJSON_AddStringToObject(data, "transport",
				mcp_transport_name(transport));
	cJSON_AddBoolToObject(data, "auto_connect", auto_connect ? 1 : 0);
	if (timeout_seconds > 0)
		cJSON_AddNumberToObject(data, "timeout_seconds",
					timeout_seconds);
	if (tools >= 0)
		cJSON_AddNumberToObject(data, "tools", tools);
	if (resources >= 0)
		cJSON_AddNumberToObject(data, "resources", resources);
	if (prompts >= 0)
		cJSON_AddNumberToObject(data, "prompts", prompts);
	if (error_code < 0) {
		cJSON_AddNumberToObject(data, "error_code", error_code);
		cJSON_AddStringToObject(data, "error",
					morph_strerror(error_code));
	}
	rc = cli_emit_event_simple(ctx, MORPH_EVENT_MCP, name, phase,
				   message, data);
	cJSON_Delete(data);
	return rc;
}

int cli_discover_mcp_server(struct cli_context *ctx,
				   struct mcp_client *mc, int auto_connect,
				   int timeout_seconds)
{
	int tools;
	int resources;
	int prompts;
	int rc = 0;
	char msg[256];

	snprintf(msg, sizeof(msg), "%s discovering capabilities",
		 mc->config.name);
	cli_emit_mcp_event(ctx, "mcp.discovering", "begin", msg,
			   mc->config.name, mc->config.transport, auto_connect,
			   timeout_seconds, -1, -1, -1, 0);

	tools = mcp_register_server_tools(mc, &ctx->tools);
	if (tools < 0)
		rc = tools;
	resources = mcp_register_server_resources(mc, &ctx->tools);
	if (resources < 0 && rc == 0)
		rc = resources;
	prompts = mcp_register_server_prompts(mc, &ctx->tools);
	if (prompts < 0 && rc == 0)
		rc = prompts;

	if (rc < 0) {
		snprintf(msg, sizeof(msg), "%s discovery failed: %s",
			 mc->config.name, morph_strerror(rc));
		cli_emit_mcp_event(ctx, "mcp.failed", "failed", msg,
				   mc->config.name, mc->config.transport,
				   auto_connect, timeout_seconds,
				   tools < 0 ? -1 : tools,
				   resources < 0 ? -1 : resources,
				   prompts < 0 ? -1 : prompts, rc);
		return rc;
	}

	snprintf(msg, sizeof(msg),
		 "%s ready (%d tools, %d resources, %d prompts)",
		 mc->config.name, tools, resources, prompts);
	cli_emit_mcp_event(ctx, "mcp.ready", "ready", msg,
			   mc->config.name, mc->config.transport, auto_connect,
			   timeout_seconds, tools, resources, prompts, 0);
	return 0;
}
