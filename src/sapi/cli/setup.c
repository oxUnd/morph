#include "setup.h"
#include "setup_ui.h"
#include "config/config.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/file.h"
#include "util/utf8.h"
#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
			MORPH_RETURN(-ECANCELED);
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
			MORPH_RETURN(rc);
		if (strlen(value) == 1 && value[0] >= '1' &&
		    value[0] <= '0' + max)
			return value[0] - '0';
		fprintf(out, "Choose a number from 1 to %d.\n", max);
	}
}

static int env_name_valid(const char *name)
{
	if (!((*name >= 'A' && *name <= 'Z') ||
	      (*name >= 'a' && *name <= 'z') || *name == '_'))
		return 0;
	for (const char *p = name + 1; *p; p++)
		if (!( (*p >= 'A' && *p <= 'Z') ||
		       (*p >= 'a' && *p <= 'z') ||
		       (*p >= '0' && *p <= '9') || *p == '_'))
			return 0;
	return 1;
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
		MORPH_RETURN(-EINVAL);
	for (const char *p = text; *p; p++)
		if (*p < '0' || *p > '9')
			MORPH_RETURN(-EINVAL);
	errno = 0;
	long parsed = strtol(text, NULL, 10);
	if (errno == ERANGE || parsed < minimum || parsed > maximum)
		MORPH_RETURN(-ERANGE);
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
		if (rc < 0) MORPH_RETURN(rc);
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
		kind == SETUP_IMAGE ? preset->image : "doubao-seedance-2-0-260128";
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
	int selected = choice(in, out, menu, "1",
		kind == SETUP_TEXT || kind == SETUP_VISION ? 4 :
		kind == SETUP_VIDEO ? 2 : 3);
	if (selected < 0)
		MORPH_RETURN(selected);
	cli_setup_model_defaults(kind, selected, entry);
	int rc = prompt(in, out, "Model name / endpoint ID", entry->model,
		entry->model, sizeof(entry->model));
	if (rc < 0)
		MORPH_RETURN(rc);
	if (kind == SETUP_TEXT || kind == SETUP_VISION) {
		cli_setup_reset_token_limits(entry);
		rc = prompt_token_count(in, out, "Context window (tokens)", 2,
			cli_setup_token_ceiling(entry, 0), &entry->context_limit);
		if (rc < 0) MORPH_RETURN(rc);
		int ceiling = cli_setup_token_ceiling(entry, 1);
		if (ceiling >= entry->context_limit) ceiling = entry->context_limit - 1;
		if (entry->max_tokens > ceiling) entry->max_tokens = 0;
		rc = prompt_token_count(in, out, "Max output (tokens)", 1, ceiling,
			&entry->max_tokens);
		if (rc < 0) MORPH_RETURN(rc);
	}
	for (;;) {
		rc = prompt(in, out, "API base URL", entry->api_base,
			entry->api_base, sizeof(entry->api_base));
		if (rc < 0)
			MORPH_RETURN(rc);
		if ((strncmp(entry->api_base, "https://", 8) == 0 &&
		     entry->api_base[8]) ||
		    (strncmp(entry->api_base, "http://", 7) == 0 &&
		     entry->api_base[7]))
			break;
		fprintf(out, "Use an http:// or https:// URL.\n");
	}
	for (;;) {
		rc = prompt(in, out, "API key environment variable (not the key)",
			entry->api_key_env,
			entry->api_key_env, sizeof(entry->api_key_env));
		if (rc < 0)
			MORPH_RETURN(rc);
		if (env_name_valid(entry->api_key_env))
			return 0;
		fprintf(out, "Use letters, digits and underscores; start with a letter or underscore.\n");
	}
}

static void append_field(morph_buf_t *buf, const char *key, const char *value)
{
	morph_buf_printf(buf, "%s = \"", key);
	for (const char *p = value; *p; p++) {
		if (*p == '\\' || *p == '"')
			morph_buf_putc(buf, '\\');
		morph_buf_putc(buf, *p);
	}
	morph_buf_puts(buf, "\"\n");
}

static int save_config(const char *path, const char *text)
{
	char *parent = strdup(path);
	if (!parent)
		MORPH_RETURN(-ENOMEM);
	char *slash = strrchr(parent, '/');
	int rc = 0;
	if (slash) {
		if (slash == parent)
			slash[1] = '\0';
		else
			*slash = '\0';
		if (strlen(parent) >= PATH_MAX)
			rc = -ENAMETOOLONG;
		else
			rc = file_ensure_dir(parent);
	}
	free(parent);
	if (rc < 0)
		MORPH_RETURN(rc);
	char *temporary = file_path_append_alloc(path, ".setup-XXXXXX");
	if (!temporary)
		MORPH_RETURN(-ENOMEM);
	int fd = mkstemp(temporary);
	if (fd < 0) {
		rc = -errno;
		free(temporary);
		MORPH_RETURN(rc);
	}
	FILE *file = fdopen(fd, "w");
	if (!file) {
		rc = -errno;
		close(fd);
	} else {
		if (fputs(text, file) == EOF)
			rc = errno ? -errno : -EIO;
		if (fclose(file) != 0 && rc == 0)
			rc = -errno;
	}
	/* Publish without replacing an existing file, including a symlink. */
	if (rc == 0 && link(temporary, path) != 0)
		rc = -errno;
	unlink(temporary);
	free(temporary);
	if (rc < 0)
		MORPH_RETURN(rc);
	return 0;
}

int cli_setup(const char *path, FILE *input, FILE *output)
{
	struct config_model_entry entries[SETUP_MODEL_COUNT] = {0};
	const char *names[] = {"text", "vision", "image", "video"};
	if (cli_setup_ui_available(input, output)) {
		int rc = cli_setup_ui_collect(path, input, output, entries);
		if (rc < 0) {
			memset(entries, 0, sizeof(entries));
			MORPH_RETURN(rc);
		}
	} else {
		fprintf(output, "Welcome to morph! Let's create %s.\n"
			"Press Enter to accept defaults; Ctrl-D cancels without saving.\n"
			"API keys stay in environment variables.\n\nText model (required)\n", path);
		for (int i = 0; i < SETUP_MODEL_COUNT; i++) {
			if (i > 0) {
				int selected = choice(input, output, i == SETUP_VISION ?
					"Configure vision (image understanding)? 1) Skip  2) Configure" :
					i == SETUP_IMAGE ?
					"Configure image generation? 1) Skip  2) Configure" :
					"Configure video generation? 1) Skip  2) Configure", "1", 2);
				if (selected < 0)
					MORPH_RETURN(selected);
				if (selected == 1)
					continue;
			}
			int rc = configure_model(input, output, i, &entries[i]);
			if (rc < 0)
				MORPH_RETURN(rc);
		}
	}
	morph_buf_t buf;
	int rc = morph_buf_init(&buf, 0);
	if (rc < 0)
		MORPH_RETURN(rc);
	morph_buf_puts(&buf, "# Created by morph first-run setup.\n");
	for (int i = 0; i < SETUP_MODEL_COUNT; i++) {
		morph_buf_printf(&buf, "\n[model.%s]\n", names[i]);
		append_field(&buf, "provider", entries[i].provider);
		append_field(&buf, "adapter", entries[i].adapter);
		append_field(&buf, "model", entries[i].model);
		append_field(&buf, "api_base", entries[i].api_base);
		append_field(&buf, "api_key_env", entries[i].api_key_env);
		if (entries[i].extra_body_json[0])
			append_field(&buf, "extra_body_json",
				     entries[i].extra_body_json);
		if ((i == SETUP_TEXT || i == SETUP_VISION) && entries[i].model[0])
			morph_buf_printf(&buf, "context_limit = %d\nmax_tokens = %d\n",
				entries[i].context_limit, entries[i].max_tokens);
	}
	struct config_validation_error error = {0};
	rc = buf.failed ? -ENOMEM : config_validate_text(morph_buf_cstr(&buf), &error);
	if (rc == 0)
		rc = save_config(path, morph_buf_cstr(&buf));
	morph_buf_cleanup(&buf);
	if (rc < 0)
		MORPH_RETURN(rc);
	fprintf(output, "\nConfiguration saved to %s.\n"
		"You can edit this file later to add or change model capabilities.\n", path);
	for (int i = 0; i < SETUP_MODEL_COUNT; i++) {
		if (entries[i].api_key[0] &&
		    setenv(entries[i].api_key_env, entries[i].api_key, 1) != 0) {
			int env_rc = -errno;
			memset(entries, 0, sizeof(entries));
			MORPH_RETURN(env_rc);
		}
	}
	int missing = 0;
	for (int i = 0; i < SETUP_MODEL_COUNT; i++) {
		if (!entries[i].model[0])
			continue;
		const char *key = getenv(entries[i].api_key_env);
		if (!key || !*key) {
			fprintf(output, "Set the %s model key in your shell:\n"
				"  export %s='<your-api-key>'\n", names[i], entries[i].api_key_env);
			missing = 1;
		}
	}
	if (missing)
		fprintf(output, "Then run morph again with the same configuration.\n");
	memset(entries, 0, sizeof(entries));
	return missing;
}

int cli_setup_if_missing(const char *path, int interactive)
{
	char *expanded = file_expand_path(path ? path : "~/.morph/config.toml");
	if (!expanded)
		MORPH_RETURN(-ENOMEM);
	struct stat st;
	if (lstat(expanded, &st) == 0) {
		free(expanded);
		return 0;
	}
	int rc = -errno;
	if (rc == -ENOENT) {
		if (interactive && isatty(STDIN_FILENO) && isatty(STDERR_FILENO)) {
			rc = cli_setup(expanded, stdin, stderr);
		} else {
			fprintf(stderr, "morph: configuration not found: %s\n"
				"Run morph in a terminal without --prompt or --events to set up"
				" (use the same -c path if specified).\n", expanded);
		}
	}
	if (rc == -ECANCELED)
		fprintf(stderr, "Setup canceled. No configuration was saved.\n");
	else if (rc < 0)
		fprintf(stderr, "morph: setup failed: %s\n", morph_strerror(rc));
	free(expanded);
	if (rc < 0)
		MORPH_RETURN(rc);
	return rc;
}
