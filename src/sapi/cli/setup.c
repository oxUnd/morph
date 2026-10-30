#include "setup.h"
#include "setup_ui.h"
#include "setup_config.h"
#include "setup_mcp.h"
#include "manage.h"
#include "config/config.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/file.h"
#include "util/utf8.h"
#include "util/name.h"
#include "cJSON.h"
#include "tomlc17.h"
#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int cli_setup_provider_choice(int kind, const struct config_model_entry *entry)
{
	if (strcmp(entry->provider, "openai") == 0 || !entry->provider[0])
		return 1;
	if (kind == SETUP_TEXT || kind == SETUP_VISION)
		return strcmp(entry->provider, "deepseek") == 0 ? 2 :
			strcmp(entry->provider, "volcengine") == 0 ? 3 : 4;
	if (kind == SETUP_IMAGE)
		return strcmp(entry->provider, "volcengine") == 0 ? 2 : 3;
	return strcmp(entry->provider, "volcengine") == 0 ? 1 : 2;
}

struct setup_preset {
	const char *provider;
	const char *base;
	const char *env;
	const char *text;
	const char *vision;
	const char *vision_base;
	const char *image;
};

static const struct setup_preset presets[] = {
	{"openai", "https://api.openai.com/v1", "OPENAI_API_KEY",
	 "gpt-4o", "gpt-4o", "https://api.openai.com/v1", "gpt-image-2"},
	{"deepseek", "https://api.deepseek.com/v1", "DEEPSEEK_API_KEY",
	 "deepseek-v4-flash", "deepseek-flash", "https://api.deepseek.com", ""},
	{"volcengine", "https://ark.cn-beijing.volces.com/api/v3",
	 "VOLCENGINE_API_KEY", "doubao-seed-2-0-lite-260428",
	 "doubao-seed-2-0-lite-260428", "https://ark.cn-beijing.volces.com/api/v3",
	 "doubao-seedream-5-0-260128"},
};

static int prompt(FILE *in, FILE *out, const char *label,
		  const char *fallback, char *value, size_t cap)
{
	for (;;) {
		char *line = NULL;
		size_t capacity = 0;
		fprintf(out, "%s%s%s%s: ", label, *fallback ? " [" : "",
			fallback, *fallback ? "]" : "");
		fflush(out);
		ssize_t len = getline(&line, &capacity, in);
		if (len < 0) {
			free(line);
			return -ECANCELED;
		}
		int embedded_nul = memchr(line, '\0', (size_t)len) != NULL;
		while (len > 0 && isspace((unsigned char)line[len - 1]))
			line[--len] = '\0';
		char *start = line;
		while (isspace((unsigned char)*start))
			start++;
		const char *chosen = *start ? start : fallback;
		int valid = !embedded_nul && *chosen && strlen(chosen) < cap &&
			utf8valid(chosen) == NULL;
		for (const char *p = chosen; *p; p++)
			if ((unsigned char)*p < 32 || *p == 127)
				valid = 0;
		if (valid)
			memmove(value, chosen, strlen(chosen) + 1);
		free(line);
		if (valid)
			return 0;
		fprintf(out, "Enter a non-empty value shorter than %zu bytes.\n", cap);
	}
}

static int choice(FILE *in, FILE *out, const char *label,
		  const char *fallback, int max)
{
	char value[16];
	for (;;) {
		int rc = prompt(in, out, label, fallback, value, sizeof(value));
		if (rc < 0)
			return rc;
		if (strlen(value) == 1 && value[0] >= '1' &&
		    value[0] <= '0' + max)
			return value[0] - '0';
		fprintf(out, "Choose a number from 1 to %d.\n", max);
	}
}

struct setup_token_limits {
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
static const struct setup_token_limits token_limits[] = {
	{"openai", "gpt-4o", 128000, 16384},
	{"deepseek", "deepseek-v4-flash", 1000000, 384000},
	{"deepseek", "deepseek-v4-pro", 1000000, 384000},
};

static const struct setup_token_limits *find_token_limits(
	const struct config_model_entry *entry)
{
	for (size_t i = 0; i < sizeof(token_limits) / sizeof(token_limits[0]); i++)
		if (strcmp(entry->provider, token_limits[i].provider) == 0 &&
		    strcmp(entry->model, token_limits[i].model) == 0)
			return &token_limits[i];
	return NULL;
}

void cli_setup_reset_token_limits(struct config_model_entry *entry)
{
	const struct setup_token_limits *limits = find_token_limits(entry);
	/* Deployment defaults, not claims about an unknown model's capacity. */
	entry->context_limit = limits ? limits->context : 128000;
	entry->max_tokens = limits ? limits->output : 16384;
	if (strcmp(entry->provider, "volcengine") == 0 &&
	    strcmp(entry->model, "doubao-seed-2-0-lite-260428") == 0)
		entry->max_tokens = 8192;
}

int cli_setup_token_ceiling(const struct config_model_entry *entry, int output)
{
	const struct setup_token_limits *limits = find_token_limits(entry);
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

static int prompt_token_count(FILE *in, FILE *out, const char *label,
			      int minimum, int maximum, int *value)
{
	char fallback[CLI_SETUP_TOKEN_INPUT_MAX] = "";
	char answer[CLI_SETUP_TOKEN_INPUT_MAX];
	if (*value > 0)
		snprintf(fallback, sizeof(fallback), "%d", *value);
	for (;;) {
		int rc = prompt(in, out, label, fallback, answer, sizeof(answer));
		if (rc < 0) return rc;
		rc = cli_setup_parse_token_count(answer, minimum, maximum, value);
		if (rc == 0) return 0;
		fprintf(out, "Enter a whole token count from %d to %d.\n", minimum, maximum);
	}
}

void cli_setup_model_defaults(int kind, int selected,
			      struct config_model_entry *entry)
{
	memset(entry, 0, sizeof(*entry));
	int custom = selected == (kind == SETUP_TEXT || kind == SETUP_VISION ? 4 :
		kind == SETUP_VIDEO ? 2 : 3);
	int index = kind == SETUP_TEXT || kind == SETUP_VISION ? selected - 1 :
		(kind == SETUP_IMAGE && selected == 1 ? 0 : 2);
	const struct setup_preset *preset = &presets[custom ? 0 : index];
	strcpy(entry->provider, custom ? "custom" : preset->provider);
	strcpy(entry->adapter, (kind == SETUP_TEXT || kind == SETUP_VISION) ?
		(!custom && index == 1 ? "deepseek" : "openai-chat-compatible") :
		kind == SETUP_IMAGE ? (!custom && index == 2 ? "volcengine-images" :
		"openai-images") : "volcengine-videos");
	const char *model = custom ? "" : kind == SETUP_TEXT ? preset->text :
		kind == SETUP_VISION ? preset->vision :
		kind == SETUP_IMAGE ? preset->image : "doubao-seedance-2-0-mini-260615";
	strcpy(entry->model, model);
	strcpy(entry->api_base, custom ? "" : kind == SETUP_VISION ?
		preset->vision_base : preset->base);
	strcpy(entry->api_key_env, custom ? "MORPH_API_KEY" : preset->env);
	if ((kind == SETUP_TEXT || kind == SETUP_VISION) &&
	    !custom && index == 1)
		strcpy(entry->extra_body_json,
		       "{\"thinking\":{\"type\":\"disabled\"}}");
	if (kind == SETUP_TEXT || kind == SETUP_VISION)
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

const char *cli_setup_model_label(int kind)
{
	const char *names[] = {"Text model", "Vision model", "Image model", "Video model"};
	return kind >= 0 && kind < SETUP_MODEL_COUNT ? names[kind] : "Model";
}

void cli_setup_prepare_model(struct cli_setup_state *state, int kind)
{
	int primary = cli_setup_provider_choice(SETUP_TEXT, &state->entries[SETUP_TEXT]);
	int provider = kind == SETUP_VIDEO ? 1 : kind == SETUP_IMAGE ?
		(primary == 3 ? 2 : 1) : primary <= 3 ? primary : 1;
	struct config_model_entry *entry = &state->entries[kind];
	cli_setup_model_defaults(kind, provider, entry);
	const struct config_model_entry *text = &state->entries[SETUP_TEXT];
	if (kind != SETUP_TEXT && strcmp(entry->provider, text->provider) == 0 &&
	    (text->api_key[0] || text->api_key_env[0])) {
		strcpy(entry->api_key, text->api_key);
		strcpy(entry->api_key_env, text->api_key[0] ? "" : text->api_key_env);
	}
}

void cli_setup_role_defaults(struct cli_setup_state *state)
{
	for (int i = 0; i < SETUP_MODEL_COUNT; i++) {
		struct config_model_entry *entry = &state->entries[i];
		if (!cli_setup_role_requires(state->role, i) || entry->provider[0]) continue;
		cli_setup_prepare_model(state, i);
	}
}

int cli_setup_model_complete(int kind, const struct config_model_entry *entry)
{
	if (!entry->model[0] || !entry->api_base[0]) return 0;
	if (!((strncmp(entry->api_base, "https://", 8) == 0 && entry->api_base[8]) ||
	      (strncmp(entry->api_base, "http://", 7) == 0 && entry->api_base[7]))) return 0;
	if (kind == SETUP_TEXT || kind == SETUP_VISION)
		return entry->context_limit >= 2 && entry->max_tokens >= 1 &&
			entry->max_tokens < entry->context_limit &&
			entry->context_limit <= cli_setup_token_ceiling(entry, 0) &&
			entry->max_tokens <= cli_setup_token_ceiling(entry, 1);
	return 1;
}

void cli_setup_share_credentials(struct cli_setup_state *state, int kind,
				 const char *previous_env)
{
	const struct config_model_entry *source = &state->entries[kind];
	if (!previous_env || !*previous_env) return;
	for (int i = 0; i < SETUP_MODEL_COUNT; i++) {
		struct config_model_entry *target = &state->entries[i];
		if (i == kind || !target->model[0] || target->api_key[0] ||
		    strcmp(target->provider, source->provider) != 0 ||
		    strcmp(target->api_key_env, previous_env) != 0) continue;
		const char *exported = getenv(target->api_key_env);
		if (exported && *exported) continue;
		strcpy(target->api_key, source->api_key);
		strcpy(target->api_key_env, source->api_key_env);
	}
}

static int configure_model(FILE *in, FILE *out, int kind,
			   struct config_model_entry *entry)
{
	const char *menu = kind == SETUP_TEXT ?
		"Provider: 1) OpenAI  2) DeepSeek  3) Volcengine  4) Other (OpenAI compatible)" :
		kind == SETUP_VISION ?
		"Provider: 1) OpenAI  2) DeepSeek  3) Volcengine  4) Other (vision chat compatible)" :
		kind == SETUP_IMAGE ?
		"Provider: 1) OpenAI  2) Volcengine  3) Other (OpenAI Images compatible)" :
		"Provider: 1) Volcengine  2) Other (Volcengine Video compatible)";
	int previous = cli_setup_provider_choice(kind, entry);
	char fallback[16];
	snprintf(fallback, sizeof(fallback), "%d", previous);
	int selected = choice(in, out, menu, fallback,
		kind == SETUP_TEXT || kind == SETUP_VISION ? 4 :
		kind == SETUP_VIDEO ? 2 : 3);
	if (selected < 0)
		return selected;
	if (!entry->provider[0] || selected != previous)
		cli_setup_model_defaults(kind, selected, entry);
	char previous_model[sizeof(entry->model)];
	strcpy(previous_model, entry->model);
	int rc = prompt(in, out, "Model name / endpoint ID", entry->model,
		entry->model, sizeof(entry->model));
	if (rc < 0)
		return rc;
	if (kind == SETUP_TEXT || kind == SETUP_VISION) {
		if (strcmp(previous_model, entry->model) != 0)
			cli_setup_reset_token_limits(entry);
		rc = prompt_token_count(in, out, "Context window (tokens)", 2,
			cli_setup_token_ceiling(entry, 0), &entry->context_limit);
		if (rc < 0) return rc;
		int ceiling = cli_setup_token_ceiling(entry, 1);
		if (ceiling >= entry->context_limit) ceiling = entry->context_limit - 1;
		if (entry->max_tokens > ceiling) entry->max_tokens = 0;
		rc = prompt_token_count(in, out, "Max output (tokens)", 1, ceiling,
			&entry->max_tokens);
		if (rc < 0) return rc;
	}
	if (strcmp(entry->adapter, "deepseek") == 0) {
		int thinking = choice(in, out,
			"DeepSeek thinking: 1) Off (faster responses)  2) On (more time to reason)",
			cli_setup_thinking_enabled(entry) ? "2" : "1", 2);

		if (thinking < 0)
			return thinking;
		rc = cli_setup_set_thinking(entry, thinking == 2);
		if (rc != 0)
			return rc;
	}
	for (;;) {
		rc = prompt(in, out, "API base URL", entry->api_base,
			entry->api_base, sizeof(entry->api_base));
		if (rc < 0)
			return rc;
		if ((strncmp(entry->api_base, "https://", 8) == 0 &&
		     entry->api_base[8]) ||
		    (strncmp(entry->api_base, "http://", 7) == 0 &&
		     entry->api_base[7]))
			break;
		fprintf(out, "Use an http:// or https:// URL.\n");
	}
	if (entry->api_key[0]) {
		memset(entry->api_key_env, 0, sizeof(entry->api_key_env));
		return 0;
	}
	for (;;) {
		rc = prompt(in, out, "API key environment variable (not the key)",
			entry->api_key_env,
			entry->api_key_env, sizeof(entry->api_key_env));
		if (rc < 0)
			return rc;
		if (morph_env_name_valid(entry->api_key_env))
			return 0;
		fprintf(out, "Use letters, digits and underscores; start with a letter or underscore.\n");
	}
}

static void append_field(morph_buf_t *buf, const char *key, const char *value)
{
	cJSON *string = cJSON_CreateString(value);
	char *encoded = string ? cJSON_PrintUnformatted(string) : NULL;
	cJSON_Delete(string);
	if (!encoded) { buf->failed = -ENOMEM; return; }
	morph_buf_printf(buf, "%s = %s\n", key, encoded);
	free(encoded);
}

static int unique_file(const char *path, const char *suffix,
		const char *text, char **created);

static int save_config(const char *path, const char *text, int replace, int *published)
{
	if (published) *published = 0;
	char *parent = strdup(path);
	if (!parent) return -ENOMEM;
	char *slash = strrchr(parent, '/');
	if (slash) {
		if (slash == parent) slash[1] = '\0';
		else *slash = '\0';
	} else strcpy(parent, ".");
	int rc = file_ensure_dir(parent);
	int directory = rc == 0 ? open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
	if (rc == 0 && directory < 0) rc = morph_error_from_errno(errno);
	free(parent);
	if (rc != 0) return rc;
	char *temporary = NULL;
	rc = unique_file(path, ".setup-XXXXXX", text, &temporary);
	if (rc == 0) {
		int result = replace ? rename(temporary, path) : link(temporary, path);
		if (result != 0) rc = morph_error_from_errno(errno);
		else if (published) *published = 1;
	}
	if (temporary) unlink(temporary);
	free(temporary);
	if (rc == 0 && fsync(directory) != 0) rc = morph_error_from_errno(errno);
	if (close(directory) != 0 && rc == 0) rc = morph_error_from_errno(errno);
	return rc;
}

static int collect_plain_custom(struct cli_setup_state *state, FILE *input, FILE *output)
{
	if (state->existing_prompt) {
		int selected = choice(input, output,
			"Instructions: 1) Keep existing  2) Use role instructions", "1", 2);
		if (selected < 0) return selected;
		state->replace_prompt = selected == 2;
	}
	for (int i = 0; i < SETUP_MODEL_COUNT; i++) {
		int required = cli_setup_role_requires(state->role, i);
		fprintf(output, "\n%s (%s)\n", cli_setup_model_label(i),
			required ? "required" : "optional");
		if (!required) {
			int selected = choice(input, output, "1) Skip  2) Configure",
				state->entries[i].model[0] ? "2" : "1", 2);
			if (selected < 0) return selected;
			if (selected == 1) {
				memset(&state->entries[i], 0, sizeof(state->entries[i]));
				continue;
			}
		}
		int rc = configure_model(input, output, i, &state->entries[i]);
		if (rc != 0) return rc;
	}
	return 0;
}

static int collect_plain(struct cli_setup_state *state, const char *path,
		FILE *input, FILE *output)
{
	fprintf(output, "Welcome to morph setup: %s.\n"
		"Choose a role, then accept the defaults or customize. Ctrl-D cancels.\n", path);
	int choose_role = 1;
	state->replace_prompt = !state->existing_prompt;
	for (;;) {
		if (choose_role) {
			fputs("\nChoose your role:\n", output);
			for (int i = 0; i < SETUP_ROLE_COUNT; i++) {
				struct cli_setup_role role = cli_setup_role_get(i);
				fprintf(output, "  %d) %s - %s\n", i + 1, role.label, role.description);
			}
			char fallback[16];
			snprintf(fallback, sizeof(fallback), "%d", state->role + 1);
			int selected = choice(input, output, "Role", fallback, SETUP_ROLE_COUNT);
			if (selected < 0) return selected;
			state->role = selected - 1;
			cli_setup_role_defaults(state);
			choose_role = 0;
		}
		fprintf(output, "\nReview your setup · %s\n", cli_setup_role_get(state->role).label);
		int complete = 1;
		for (int i = 0; i < SETUP_MODEL_COUNT; i++) {
			struct config_model_entry *entry = &state->entries[i];
			int required = cli_setup_role_requires(state->role, i);
			fprintf(output, "%s (%s): %s\n", cli_setup_model_label(i),
				required ? "required" : "optional", entry->model[0] ? entry->model : "Off");
			if ((required || entry->model[0]) && !cli_setup_model_complete(i, entry))
				complete = 0;
		}
		fprintf(output, "%s\nRead-only/media tools: auto-approved. Host execution: enabled.\n"
			"Instructions: %s.\n", cli_setup_mcp_summary(state->role),
			state->replace_prompt ? "Use role prompt" : "Keep existing");
		int selected = choice(input, output,
			"1) Save and exit  2) Customize  3) Change role  4) Cancel", "1", 4);
		if (selected < 0) return selected;
		if (selected == 4) return -ECANCELED;
		if (selected == 1 && complete) return 0;
		if (selected == 1) {
			fputs("Complete the model name, API URL and valid token limits before saving.\n", output);
			continue;
		}
		if (selected == 3) { choose_role = 1; continue; }
		int rc = collect_plain_custom(state, input, output);
		if (rc != 0) return rc;
	}
}

static int load_existing(const char *path, struct cli_setup_state *state, char **original)
{
	struct stat st;
	if (lstat(path, &st) != 0)
		return errno == ENOENT ? 0 : morph_error_from_errno(errno);
	if (!S_ISREG(st.st_mode)) return -EINVAL;
	if (!state->reconfigure) return -EEXIST;
	if (st.st_size > 1024 * 1024) return -EFBIG;
	size_t len;
	*original = file_read_all(path, &len);
	if (!*original) return morph_error_from_errno(errno);
	if (memchr(*original, '\0', len) || utf8nvalid(*original, len)) return -EILSEQ;
	struct config *config = calloc(1, sizeof(*config));
	if (!config) return -ENOMEM;
	int rc = config_load(NULL, config, path);
	if (rc == 0) {
		const struct config_model_entry *models[] = {&config->models.text,
			&config->models.vision, &config->models.image, &config->models.video};
		const char *names[] = {"text", "vision", "image", "video"};
		toml_result_t parsed = toml_parse(*original, (int)len);
		toml_datum_t model = toml_get(parsed.toptab, "model");
		for (int i = 0; i < SETUP_MODEL_COUNT; i++)
			if (toml_get(model, names[i]).type == TOML_TABLE)
				state->entries[i] = *models[i];
		toml_free(parsed);
		state->role = cli_setup_config_role(*original);
		state->existing_config = 1;
		state->existing_prompt = config->prompt.system_prompt_file[0] ||
			config->prompt.system_prompt_dir[0];
	}
	memset(config, 0, sizeof(*config));
	free(config);
	return rc;
}

static int unique_file(const char *path, const char *suffix, const char *text, char **created)
{
	*created = file_path_append_alloc(path, suffix);
	if (!*created) return -ENOMEM;
	size_t suffix_len = strlen(suffix);
	int markdown = suffix_len >= 3 && strcmp(suffix + suffix_len - 3, ".md") == 0;
	int fd = markdown ? mkstemps(*created, 3) : mkstemp(*created);
	if (fd < 0) {
		int rc = morph_error_from_errno(errno);
		free(*created);
		*created = NULL;
		return rc;
	}
	size_t offset = 0;
	size_t len = strlen(text);
	int rc = 0;
	while (offset < len) {
		ssize_t n = write(fd, text + offset, len - offset);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) { rc = morph_error_from_errno(errno); break; }
		offset += (size_t)n;
	}
	if (rc == 0 && fsync(fd) != 0) rc = morph_error_from_errno(errno);
	if (close(fd) != 0 && rc == 0) rc = morph_error_from_errno(errno);
	if (rc != 0) {
		unlink(*created);
		free(*created);
		*created = NULL;
	}
	return rc;
}

static int role_snapshot(const char *path, int role, const char *text,
		char **snapshot, int *created)
{
	*snapshot = NULL;
	*created = 0;
	char *directory = file_path_append_alloc(path, ".prompts");
	if (!directory) return -ENOMEM;
	int rc = file_ensure_dir(directory);
	char *base = rc == 0 ? file_path_join_alloc(directory, cli_setup_role_get(role).id) : NULL;
	char *named = base ? file_path_append_alloc(base, ".md") : NULL;
	if (rc == 0 && !named) rc = -ENOMEM;
	if (rc == 0) {
		struct stat st;
		if (lstat(named, &st) == 0 && S_ISREG(st.st_mode) &&
		    st.st_size >= 0 && (size_t)st.st_size == strlen(text)) {
			char *current = file_read_all(named, NULL);
			int same = current && strcmp(current, text) == 0;
			free(current);
			if (same) {
				*snapshot = named;
				free(base);
				free(directory);
				return 0;
			}
		}
		rc = save_config(named, text, 0, created);
		if (rc == -EEXIST) {
			/* Preserve a customized or concurrently published named snapshot. */
			rc = unique_file(base, "-XXXXXX.md", text, snapshot);
			if (rc == 0) {
				*created = 1;
				int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
				if (fd < 0) rc = morph_error_from_errno(errno);
				else {
					if (fsync(fd) != 0) rc = morph_error_from_errno(errno);
					if (close(fd) != 0 && rc == 0) rc = morph_error_from_errno(errno);
				}
			}
		} else if (rc == 0 || *created) {
			*snapshot = named;
			named = NULL;
		}
	}
	free(named);
	free(base);
	free(directory);
	return rc;
}

static int publish_setup(const char *path, const char *original, const char *text, FILE *output)
{
	if (!original) return save_config(path, text, 0, NULL);
	char *lock_path = file_path_append_alloc(path, ".lock");
	if (!lock_path) return -ENOMEM;
	int lock = cli_manage_lock(lock_path);
	free(lock_path);
	if (lock < 0) return lock;
	struct stat st;
	int rc = lstat(path, &st) == 0 ? 0 : morph_error_from_errno(errno);
	if (rc == 0 && !S_ISREG(st.st_mode)) rc = -EINVAL;
	char *current = rc == 0 ? file_read_all(path, NULL) : NULL;
	if (rc == 0 && (!current || strcmp(current, original) != 0)) rc = -ESTALE;
	free(current);
	char *backup = NULL;
	if (rc == 0) rc = unique_file(path, ".backup-XXXXXX", original, &backup);
	if (rc == 0) rc = save_config(path, text, 1, NULL);
	if (backup) cli_setup_print(output, "Previous configuration backed up to %s.\n", backup);
	free(backup);
	close(lock);
	return rc;
}

static int save_setup(struct cli_setup_state *state, const char *path,
		const char *original, FILE *output)
{
	const char *names[] = {"text", "vision", "image", "video"};
	for (int i = 0; i < SETUP_MODEL_COUNT; i++)
		if (cli_setup_role_requires(state->role, i) &&
		    !cli_setup_model_complete(i, &state->entries[i]))
			return MORPH_ERR_NOT_CONFIGURED;
	char *prompt_text = NULL;
	char *snapshot = NULL;
	int snapshot_created = 0;
	char *merged = NULL;
	morph_buf_t patch = {0};
	int rc = morph_buf_init(&patch, 0);
	if (rc != 0) return rc;
	if (state->replace_prompt) {
		rc = cli_setup_role_render(state->role, &prompt_text);
		if (rc == 0) rc = role_snapshot(path, state->role, prompt_text,
			&snapshot, &snapshot_created);
		if (rc == 0) {
			morph_buf_puts(&patch, "[prompt]\nmode = \"replace\"\n");
			append_field(&patch, "system_prompt_file", snapshot);
			append_field(&patch, "system_prompt_dir", "");
		}
	}
	morph_buf_puts(&patch, "\n[setup]\n");
	append_field(&patch, "role", cli_setup_role_get(state->role).id);
	for (int i = 0; i < SETUP_MODEL_COUNT; i++) {
		struct config_model_entry *entry = &state->entries[i];
		morph_buf_printf(&patch, "\n[model.%s]\n", names[i]);
		append_field(&patch, "provider", entry->provider);
		append_field(&patch, "adapter", entry->adapter);
		append_field(&patch, "model", entry->model);
		append_field(&patch, "api_base", entry->api_base);
		append_field(&patch, "api_key", entry->api_key);
		append_field(&patch, "api_key_env", entry->api_key_env);
		append_field(&patch, "extra_body_json", entry->extra_body_json);
		if ((i == SETUP_TEXT || i == SETUP_VISION) && entry->model[0])
			morph_buf_printf(&patch, "context_limit = %d\nmax_tokens = %d\n",
				entry->context_limit, entry->max_tokens);
	}
	if (!original) morph_buf_puts(&patch, "\n[exec]\nnetwork = true\n");
	else morph_buf_puts(&patch, "\n[exec]\n");
	morph_buf_puts(&patch, "allow_host_execution = true\n"
		"\n[react]\nhitl_auto_approve_readonly = true\nhitl_auto_approve_media = true\n");
	if (rc == 0) rc = patch.failed;
	if (rc == 0) rc = cli_setup_config_merge(original ? original : "",
		morph_buf_cstr(&patch), &merged);
	if (rc == 0) {
		char *credentials = NULL;
		rc = cli_setup_config_credentials(merged, &credentials);
		free(merged);
		merged = credentials;
	}
	if (rc == 0) {
		char *mcp = NULL;
		rc = cli_setup_mcp_apply(merged, state->role, &mcp);
		free(merged);
		merged = mcp;
	}
	struct config_validation_error error = {0};
	if (rc == 0) rc = config_validate_text(merged, &error);
	if (rc == 0) rc = publish_setup(path, original, merged, output);
	if (rc != 0 && snapshot && snapshot_created) {
		/* A directory sync failure can follow a successful atomic replacement. */
		char *published = file_read_all(path, NULL);
		if (!published || !merged || strcmp(published, merged) != 0) unlink(snapshot);
		free(published);
	}
	free(prompt_text);
	free(snapshot);
	free(merged);
	morph_buf_cleanup(&patch);
	return rc;
}

static int setup_run(struct cli_context *ctx, const char *path,
		FILE *input, FILE *output, int reconfigure)
{
	struct cli_setup_state state = {.reconfigure = reconfigure};
	char *original = NULL;
	char current[PATH_MAX];
	char *expanded = path ? file_expand_path(path) : NULL;
	char *full = expanded && getcwd(current, sizeof(current)) ?
		file_path_full_alloc(current, expanded) : NULL;
	free(expanded);
	if (!full) return -EINVAL;
	int rc = load_existing(full, &state, &original);
	if (rc == 0) rc = cli_setup_ui_available(input, output) ?
		cli_setup_ui_collect(ctx, full, input, output, &state) :
		collect_plain(&state, full, input, output);
	if (rc == 0) rc = save_setup(&state, full, original, output);
	if (rc == 0) {
		cli_setup_print(output, "\nConfiguration saved to %s.\n", full);
		cli_setup_mcp_notes(state.role, output);
		for (int i = 0; i < SETUP_MODEL_COUNT; i++) {
			struct config_model_entry *entry = &state.entries[i];
			if (!entry->model[0] || entry->api_key[0]) continue;
			const char *key = getenv(entry->api_key_env);
			if (!key || !*key) {
				rc = 1;
				int shown = 0;
				for (int previous = 0; previous < i; previous++)
					if (state.entries[previous].model[0] && !state.entries[previous].api_key[0] &&
					    strcmp(state.entries[previous].api_key_env, entry->api_key_env) == 0)
						shown = 1;
				if (shown) continue;
				cli_setup_print(output, "Set the %s model key in your shell:\n"
					"  export %s='<your-api-key>'\n",
					i == SETUP_TEXT ? "text" : i == SETUP_VISION ? "vision" :
					i == SETUP_IMAGE ? "image" : "video", entry->api_key_env);
				rc = 1;
			}
		}
		cli_setup_print(output,
			"Setup complete. Run morph again with the same -c configuration to start.\n"
			"To change these settings, run morph setup with the same -c path.\n");
	}
	memset(&state, 0, sizeof(state));
	free(original);
	free(full);
	return rc;
}

int cli_setup(struct cli_context *ctx, const char *path, FILE *input, FILE *output)
{
	return setup_run(ctx, path, input, output, 0);
}

int cli_setup_reconfigure(struct cli_context *ctx, const char *path, FILE *input, FILE *output)
{
	return setup_run(ctx, path, input, output, 1);
}

int cli_setup_command(struct cli_context *ctx, int argc, char **argv, const char *path)
{
	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			fputs("Usage: morph setup [-c PATH]\n"
				"Choose a role, configure its models, save and exit.\n", stdout);
			return 0;
		}
		if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc)
			path = argv[++i];
		else if (strncmp(argv[i], "--config=", 9) == 0 && argv[i][9])
			path = argv[i] + 9;
		else {
			fprintf(stderr, "morph: unexpected setup argument: %s\n", argv[i]);
			return 2;
		}
	}
	int rc = cli_setup_reconfigure(ctx, path ? path : "~/.morph/config.toml", stdin, stderr);
	if (rc < 0) fprintf(stderr, "morph: setup failed: %s\n", morph_strerror(rc));
	return rc < 0 ? 1 : 0;
}

int cli_setup_if_missing(struct cli_context *ctx, const char *path, int interactive)
{
	char *expanded = file_expand_path(path ? path : "~/.morph/config.toml");
	if (!expanded)
		return -ENOMEM;
	struct stat st;
	if (lstat(expanded, &st) == 0) {
		free(expanded);
		return 0;
	}
	int rc = -errno;
	if (rc == -ENOENT) {
		if (interactive && isatty(STDIN_FILENO) && isatty(STDERR_FILENO)) {
			rc = cli_setup(ctx, expanded, stdin, stderr);
			if (rc >= 0) rc = 1;
		} else {
			fprintf(stderr, "morph: configuration not found: %s\n"
				"Run morph setup to configure it"
				" (use the same -c path if specified).\n", expanded);
		}
	}
	if (rc == -ECANCELED)
		fprintf(stderr, "Setup canceled. No configuration was saved.\n");
	else if (rc < 0)
		fprintf(stderr, "morph: setup failed: %s\n", morph_strerror(rc));
	free(expanded);
	if (rc < 0)
		return rc;
	return rc;
}
