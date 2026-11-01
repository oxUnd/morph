#include "sapi/cli/services/configuration/model.h"
#include "sapi/cli/services/configuration/role.h"
#include "util/error.h"
#include "cJSON.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cli_model_setup_preset {
	const char *provider;
	const char *base;
	const char *env;
	const char *text;
	const char *vision;
	const char *vision_base;
	const char *image;
};

static const struct cli_model_setup_preset cli_model_presets[] = {
	{"openai", "https://api.openai.com/v1", "OPENAI_API_KEY", "gpt-4o", "gpt-4o",
	 "https://api.openai.com/v1", "gpt-image-2"},
	{"deepseek", "https://api.deepseek.com/v1", "DEEPSEEK_API_KEY", "deepseek-v4-flash",
	 "deepseek-flash", "https://api.deepseek.com", ""},
	{"volcengine", "https://ark.cn-beijing.volces.com/api/v3", "VOLCENGINE_API_KEY",
	 "doubao-seed-2-0-lite-260428", "doubao-seed-2-0-lite-260428",
	 "https://ark.cn-beijing.volces.com/api/v3", "doubao-seedream-5-0-260128"},
};

struct cli_model_setup_token_limits {
	const char *provider;
	const char *model;
	int context;
	int output;
};

/* Verified 2026-09-21 against official model / integration documentation:
 * https://developers.openai.com/api/docs/models/gpt-4o
 * https://api-docs.deepseek.com/quick_start/agent_integrations/pi_mono/
 * Match exact model IDs; do not infer limits from a provider or prefix.
 */
static const struct cli_model_setup_token_limits cli_model_token_limits[] = {
	{"openai", "gpt-4o", 128000, 16384},
	{"deepseek", "deepseek-v4-flash", 1000000, 384000},
	{"deepseek", "deepseek-v4-pro", 1000000, 384000},
};

static const struct cli_model_setup_token_limits *
cli_model_find_token_limits(const struct config_model_entry *entry)
{
	for (size_t i = 0; i < sizeof(cli_model_token_limits) / sizeof(cli_model_token_limits[0]);
	     i++)
		if (strcmp(entry->provider, cli_model_token_limits[i].provider) == 0 &&
		    strcmp(entry->model, cli_model_token_limits[i].model) == 0)
			return &cli_model_token_limits[i];
	return NULL;
}

void cli_setup_model_defaults(int kind, int selected, struct config_model_entry *entry)
{
	memset(entry, 0, sizeof(*entry));
	int custom = selected == (kind == CLI_SETUP_TEXT || kind == CLI_SETUP_VISION ? 4
				  : kind == CLI_SETUP_VIDEO			     ? 2
										     : 3);
	int index = kind == CLI_SETUP_TEXT || kind == CLI_SETUP_VISION
			    ? selected - 1
			    : (kind == CLI_SETUP_IMAGE && selected == 1 ? 0 : 2);
	const struct cli_model_setup_preset *preset = &cli_model_presets[custom ? 0 : index];
	strcpy(entry->provider, custom ? "custom" : preset->provider);
	strcpy(entry->adapter,
	       (kind == CLI_SETUP_TEXT || kind == CLI_SETUP_VISION)
		       ? (!custom && index == 1 ? "deepseek" : "openai-chat-compatible")
	       : kind == CLI_SETUP_IMAGE
		       ? (!custom && index == 2 ? "volcengine-images" : "openai-images")
		       : "volcengine-videos");
	const char *model = custom		       ? ""
			    : kind == CLI_SETUP_TEXT   ? preset->text
			    : kind == CLI_SETUP_VISION ? preset->vision
			    : kind == CLI_SETUP_IMAGE  ? preset->image
						       : "doubao-seedance-2-0-mini-260615";
	strcpy(entry->model, model);
	strcpy(entry->api_base, custom			   ? ""
				: kind == CLI_SETUP_VISION ? preset->vision_base
							   : preset->base);
	strcpy(entry->api_key_env, custom ? "MORPH_API_KEY" : preset->env);
	if ((kind == CLI_SETUP_TEXT || kind == CLI_SETUP_VISION) && !custom && index == 1)
		strcpy(entry->extra_body_json, "{\"thinking\":{\"type\":\"disabled\"}}");
	if (kind == CLI_SETUP_TEXT || kind == CLI_SETUP_VISION)
		cli_setup_reset_token_limits(entry);
}

int cli_setup_thinking_enabled(const struct config_model_entry *entry)
{
	cJSON *root = cJSON_Parse(entry->extra_body_json);
	const cJSON *thinking = cJSON_GetObjectItemCaseSensitive(root, "thinking");
	const cJSON *type = cJSON_GetObjectItemCaseSensitive(thinking, "type");
	int enabled = cJSON_IsString(type) && strcmp(type->valuestring, "enabled") == 0;

	cJSON_Delete(root);
	return enabled;
}

int cli_setup_set_thinking(struct config_model_entry *entry, int enabled)
{
	cJSON *root;
	cJSON *thinking;
	char *json;
	int rc = 0;

	if (strcmp(entry->adapter, "deepseek") != 0)
		return -EINVAL;
	if (entry->extra_body_json[0]) {
		root = cJSON_ParseWithOpts(entry->extra_body_json, NULL, 1);
		if (!cJSON_IsObject(root)) {
			cJSON_Delete(root);
			return MORPH_ERR_PARSE;
		}
	} else {
		root = cJSON_CreateObject();
		if (!root)
			return -ENOMEM;
	}
	thinking = cJSON_GetObjectItemCaseSensitive(root, "thinking");
	if (!cJSON_IsObject(thinking)) {
		cJSON_DeleteItemFromObjectCaseSensitive(root, "thinking");
		thinking = cJSON_AddObjectToObject(root, "thinking");
	}
	if (!thinking) {
		cJSON_Delete(root);
		return -ENOMEM;
	}
	cJSON_DeleteItemFromObjectCaseSensitive(thinking, "type");
	if (!cJSON_AddStringToObject(thinking, "type", enabled ? "enabled" : "disabled")) {
		cJSON_Delete(root);
		return -ENOMEM;
	}
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json)
		return -ENOMEM;
	if (strlen(json) >= sizeof(entry->extra_body_json))
		rc = (-E2BIG);
	else
		strcpy(entry->extra_body_json, json);
	free(json);
	return rc;
}

void cli_setup_reset_token_limits(struct config_model_entry *entry)
{
	const struct cli_model_setup_token_limits *limits = cli_model_find_token_limits(entry);
	/* Deployment defaults, not claims about an unknown model's capacity. */
	entry->context_limit = limits ? limits->context : 128000;
	entry->max_tokens = limits ? limits->output : 16384;
	if (strcmp(entry->provider, "volcengine") == 0 &&
	    strcmp(entry->model, "doubao-seed-2-0-lite-260428") == 0)
		entry->max_tokens = 8192;
}

int cli_setup_token_ceiling(const struct config_model_entry *entry, int output)
{
	const struct cli_model_setup_token_limits *limits = cli_model_find_token_limits(entry);
	return limits ? (output ? limits->output : limits->context) : INT_MAX;
}

int cli_setup_parse_token_count(const char *text, int minimum, int maximum, int *value)
{
	if (!text || !*text || !value)
		return -EINVAL;
	for (const char *p = text; *p; p++)
		if (*p < '0' || *p > '9')
			return -EINVAL;
	errno = 0;
	long parsed = strtol(text, NULL, 10);
	if (errno == ERANGE || parsed < minimum || parsed > maximum)
		return -ERANGE;
	*value = (int)parsed;
	return 0;
}

int cli_setup_provider_choice(int kind, const struct config_model_entry *entry)
{
	if (strcmp(entry->provider, "openai") == 0 || !entry->provider[0])
		return 1;
	if (kind == CLI_SETUP_TEXT || kind == CLI_SETUP_VISION)
		return strcmp(entry->provider, "deepseek") == 0	    ? 2
		       : strcmp(entry->provider, "volcengine") == 0 ? 3
								    : 4;
	if (kind == CLI_SETUP_IMAGE)
		return strcmp(entry->provider, "volcengine") == 0 ? 2 : 3;
	return strcmp(entry->provider, "volcengine") == 0 ? 1 : 2;
}

const char *cli_setup_model_label(int kind)
{
	const char *names[] = {"Text model", "Vision model", "Image model", "Video model"};
	return kind >= 0 && kind < CLI_SETUP_MODEL_COUNT ? names[kind] : "Model";
}

void cli_setup_prepare_model(struct cli_setup_state *state, int kind)
{
	int primary = cli_setup_provider_choice(CLI_SETUP_TEXT, &state->entries[CLI_SETUP_TEXT]);
	int provider = kind == CLI_SETUP_VIDEO	 ? 1
		       : kind == CLI_SETUP_IMAGE ? (primary == 3 ? 2 : 1)
		       : primary <= 3		 ? primary
						 : 1;
	struct config_model_entry *entry = &state->entries[kind];
	cli_setup_model_defaults(kind, provider, entry);
	const struct config_model_entry *text = &state->entries[CLI_SETUP_TEXT];
	if (kind != CLI_SETUP_TEXT && strcmp(entry->provider, text->provider) == 0 &&
	    (text->api_key[0] || text->api_key_env[0])) {
		strcpy(entry->api_key, text->api_key);
		strcpy(entry->api_key_env, text->api_key[0] ? "" : text->api_key_env);
	}
}

void cli_setup_role_defaults(struct cli_setup_state *state)
{
	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++) {
		struct config_model_entry *entry = &state->entries[i];
		if (!cli_setup_role_requires(state->role, i) || entry->provider[0])
			continue;
		cli_setup_prepare_model(state, i);
	}
}

void cli_setup_share_credentials(struct cli_setup_state *state, int kind, const char *previous_env)
{
	const struct config_model_entry *source = &state->entries[kind];
	if (!previous_env || !*previous_env)
		return;
	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++) {
		struct config_model_entry *target = &state->entries[i];
		if (i == kind || !target->model[0] || target->api_key[0] ||
		    strcmp(target->provider, source->provider) != 0 ||
		    strcmp(target->api_key_env, previous_env) != 0)
			continue;
		const char *exported = getenv(target->api_key_env);
		if (exported && *exported)
			continue;
		strcpy(target->api_key, source->api_key);
		strcpy(target->api_key_env, source->api_key_env);
	}
}

int cli_setup_model_complete(int kind, const struct config_model_entry *entry)
{
	if (!entry->model[0] || !entry->api_base[0])
		return 0;
	if (!((strncmp(entry->api_base, "https://", 8) == 0 && entry->api_base[8]) ||
	      (strncmp(entry->api_base, "http://", 7) == 0 && entry->api_base[7])))
		return 0;
	if (kind == CLI_SETUP_TEXT || kind == CLI_SETUP_VISION)
		return entry->context_limit >= 2 && entry->max_tokens >= 1 &&
		       entry->max_tokens < entry->context_limit &&
		       entry->context_limit <= cli_setup_token_ceiling(entry, 0) &&
		       entry->max_tokens <= cli_setup_token_ceiling(entry, 1);
	return 1;
}
