#include "sapi/cli/internal.h"

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
				env_s = (*runtime_config_get(ctx->runtime)).models.image.api_key_env;
			else if (strcmp(backend_s, "video") == 0)
				env_s = (*runtime_config_get(ctx->runtime)).models.video.api_key_env;
			else
				env_s = (*runtime_config_get(ctx->runtime)).models.text.api_key_env;
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
			       runtime_config_path_get(ctx->runtime)[0] ? runtime_config_path_get(ctx->runtime) :
			       default_config_path, env_s);
		} else {
			printf("Edit %s, then retry.\n",
			       runtime_config_path_get(ctx->runtime)[0] ? runtime_config_path_get(ctx->runtime) :
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
