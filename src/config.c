#include "config.h"
#include "util/log.h"
#include "util/file.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

void config_set_defaults(struct config *cfg)
{
	if (!cfg)
		return;
	memset(cfg, 0, sizeof(*cfg));

	strncpy(cfg->general.default_session, "default",
		sizeof(cfg->general.default_session) - 1);
	strncpy(cfg->general.output_dir, "~/.multi-agent/output",
		sizeof(cfg->general.output_dir) - 1);
	strncpy(cfg->general.log_level, "info",
		sizeof(cfg->general.log_level) - 1);
	strncpy(cfg->general.log_file, "~/.multi-agent/log/agent.log",
		sizeof(cfg->general.log_file) - 1);

	strncpy(cfg->models.text.provider, "openai",
		sizeof(cfg->models.text.provider) - 1);
	strncpy(cfg->models.text.model, "gpt-4o",
		sizeof(cfg->models.text.model) - 1);
	strncpy(cfg->models.text.api_base, "https://api.openai.com/v1",
		sizeof(cfg->models.text.api_base) - 1);
	strncpy(cfg->models.text.api_key_env, "OPENAI_API_KEY",
		sizeof(cfg->models.text.api_key_env) - 1);
	cfg->models.text.context_limit = 128000;
	cfg->models.text.timeout_seconds = 60;

	strncpy(cfg->models.image.provider, "openai",
		sizeof(cfg->models.image.provider) - 1);
	strncpy(cfg->models.image.model, "dall-e-3",
		sizeof(cfg->models.image.model) - 1);
	strncpy(cfg->models.image.api_base, "https://api.openai.com/v1",
		sizeof(cfg->models.image.api_base) - 1);
	strncpy(cfg->models.image.api_key_env, "OPENAI_API_KEY",
		sizeof(cfg->models.image.api_key_env) - 1);

	strncpy(cfg->models.video.provider, "volcengine",
		sizeof(cfg->models.video.provider) - 1);
	strncpy(cfg->models.video.model, "",
		sizeof(cfg->models.video.model) - 1);
	strncpy(cfg->models.video.api_base, "https://ark.cn-beijing.volces.com/api/v3",
		sizeof(cfg->models.video.api_base) - 1);
	strncpy(cfg->models.video.api_key_env, "VOLCENGINE_API_KEY",
		sizeof(cfg->models.video.api_key_env) - 1);
	cfg->models.video.poll_interval_seconds = 5;
	cfg->models.video.poll_timeout_seconds = 600;

	cfg->react.max_iterations = 10;
	cfg->react.step_timeout_seconds = 60;
	cfg->react.tool_max_retries = 3;

	cfg->context.summarize_threshold_ratio = 0.8;
	cfg->context.compress_target_ratio = 0.5;
	cfg->context.keep_recent_rounds = 6;

	strncpy(cfg->render.prefer_image_protocol, "auto",
		sizeof(cfg->render.prefer_image_protocol) - 1);
	strncpy(cfg->render.mpv_args, "--really-quiet",
		sizeof(cfg->render.mpv_args) - 1);

	strncpy(cfg->skill.dir, "~/.multi-agent/skills",
		sizeof(cfg->skill.dir) - 1);
	cfg->skill.default_max_memory_mb = 128;
	cfg->skill.default_max_cpu_seconds = 30;
}

static char *trim(char *s)
{
	while (*s && isspace((unsigned char)*s))
		s++;
	char *end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end))
		*end-- = '\0';
	return s;
}

static int parse_bool(const char *s)
{
	return (strcasecmp(s, "true") == 0 || strcasecmp(s, "1") == 0);
}

static char *unquote(char *s)
{
	size_t len = strlen(s);
	if (len >= 2 && ((s[0] == '"' && s[len - 1] == '"') ||
			 (s[0] == '\'' && s[len - 1] == '\''))) {
		s[len - 1] = '\0';
		memmove(s, s + 1, len - 1);
	}
	return s;
}

static int find_section(const char *line, char *section, size_t section_len)
{
	const char *p = strchr(line, '[');
	if (!p)
		return 0;
	const char *e = strchr(p, ']');
	if (!e)
		return 0;
	size_t len = (size_t)(e - p - 1);
	if (len >= section_len)
		len = section_len - 1;
	memcpy(section, p + 1, len);
	section[len] = '\0';
	return 1;
}

static void apply_kv(struct config *cfg, const char *section,
		     const char *key, const char *value)
{
	if (strcmp(section, "general") == 0) {
		if (strcmp(key, "default_session") == 0)
			strncpy(cfg->general.default_session, value,
				sizeof(cfg->general.default_session) - 1);
		else if (strcmp(key, "output_dir") == 0)
			strncpy(cfg->general.output_dir, value,
				sizeof(cfg->general.output_dir) - 1);
		else if (strcmp(key, "log_level") == 0)
			strncpy(cfg->general.log_level, value,
				sizeof(cfg->general.log_level) - 1);
		else if (strcmp(key, "log_file") == 0)
			strncpy(cfg->general.log_file, value,
				sizeof(cfg->general.log_file) - 1);
	} else if (strcmp(section, "model.text") == 0) {
		if (strcmp(key, "provider") == 0)
			strncpy(cfg->models.text.provider, value,
				sizeof(cfg->models.text.provider) - 1);
		else if (strcmp(key, "model") == 0)
			strncpy(cfg->models.text.model, value,
				sizeof(cfg->models.text.model) - 1);
		else if (strcmp(key, "api_base") == 0)
			strncpy(cfg->models.text.api_base, value,
				sizeof(cfg->models.text.api_base) - 1);
		else if (strcmp(key, "api_key_env") == 0)
			strncpy(cfg->models.text.api_key_env, value,
				sizeof(cfg->models.text.api_key_env) - 1);
		else if (strcmp(key, "api_key") == 0)
			strncpy(cfg->models.text.api_key, value,
				sizeof(cfg->models.text.api_key) - 1);
		else if (strcmp(key, "context_limit") == 0)
			cfg->models.text.context_limit = atoi(value);
		else if (strcmp(key, "timeout_seconds") == 0)
			cfg->models.text.timeout_seconds = atoi(value);
	} else if (strcmp(section, "model.image") == 0) {
		if (strcmp(key, "provider") == 0)
			strncpy(cfg->models.image.provider, value,
				sizeof(cfg->models.image.provider) - 1);
		else if (strcmp(key, "model") == 0)
			strncpy(cfg->models.image.model, value,
				sizeof(cfg->models.image.model) - 1);
		else if (strcmp(key, "api_base") == 0)
			strncpy(cfg->models.image.api_base, value,
				sizeof(cfg->models.image.api_base) - 1);
		else if (strcmp(key, "api_key_env") == 0)
			strncpy(cfg->models.image.api_key_env, value,
				sizeof(cfg->models.image.api_key_env) - 1);
		else if (strcmp(key, "api_key") == 0)
			strncpy(cfg->models.image.api_key, value,
				sizeof(cfg->models.image.api_key) - 1);
	} else if (strcmp(section, "model.video") == 0) {
		if (strcmp(key, "provider") == 0)
			strncpy(cfg->models.video.provider, value,
				sizeof(cfg->models.video.provider) - 1);
		else if (strcmp(key, "model") == 0)
			strncpy(cfg->models.video.model, value,
				sizeof(cfg->models.video.model) - 1);
		else if (strcmp(key, "api_base") == 0)
			strncpy(cfg->models.video.api_base, value,
				sizeof(cfg->models.video.api_base) - 1);
		else if (strcmp(key, "api_key_env") == 0)
			strncpy(cfg->models.video.api_key_env, value,
				sizeof(cfg->models.video.api_key_env) - 1);
		else if (strcmp(key, "api_key") == 0)
			strncpy(cfg->models.video.api_key, value,
				sizeof(cfg->models.video.api_key) - 1);
		else if (strcmp(key, "poll_interval_seconds") == 0)
			cfg->models.video.poll_interval_seconds = atoi(value);
		else if (strcmp(key, "poll_timeout_seconds") == 0)
			cfg->models.video.poll_timeout_seconds = atoi(value);
	} else if (strcmp(section, "react") == 0) {
		if (strcmp(key, "max_iterations") == 0)
			cfg->react.max_iterations = atoi(value);
		else if (strcmp(key, "step_timeout_seconds") == 0)
			cfg->react.step_timeout_seconds = atoi(value);
		else if (strcmp(key, "tool_max_retries") == 0)
			cfg->react.tool_max_retries = atoi(value);
	} else if (strcmp(section, "context") == 0) {
		if (strcmp(key, "summarize_threshold_ratio") == 0)
			cfg->context.summarize_threshold_ratio = atof(value);
		else if (strcmp(key, "compress_target_ratio") == 0)
			cfg->context.compress_target_ratio = atof(value);
		else if (strcmp(key, "keep_recent_rounds") == 0)
			cfg->context.keep_recent_rounds = atoi(value);
	} else if (strcmp(section, "render") == 0) {
		if (strcmp(key, "prefer_image_protocol") == 0)
			strncpy(cfg->render.prefer_image_protocol, value,
				sizeof(cfg->render.prefer_image_protocol) - 1);
		else if (strcmp(key, "mpv_args") == 0)
			strncpy(cfg->render.mpv_args, value,
				sizeof(cfg->render.mpv_args) - 1);
	} else if (strcmp(section, "skill") == 0) {
		if (strcmp(key, "dir") == 0)
			strncpy(cfg->skill.dir, value,
				sizeof(cfg->skill.dir) - 1);
		else if (strcmp(key, "default_max_memory_mb") == 0)
			cfg->skill.default_max_memory_mb = atoi(value);
		else if (strcmp(key, "default_max_cpu_seconds") == 0)
			cfg->skill.default_max_cpu_seconds = atoi(value);
	}
}

int config_load(struct config *cfg, const char *path)
{
	if (!cfg || !path)
		return -EINVAL;
	config_set_defaults(cfg);

	size_t len = 0;
	char *data = file_read_all(path, &len);
	if (!data) {
		log_warn("config file not found: %s, using defaults", path);
		return 0;
	}

	char section[128] = "general";
	char *line = data;
	while (line && *line) {
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';
		char *trimmed = trim(line);
		if (*trimmed == '\0' || *trimmed == '#') {
			line = nl ? nl + 1 : NULL;
			continue;
		}
		char sec[128];
		if (find_section(trimmed, sec, sizeof(sec))) {
			strncpy(section, sec, sizeof(section) - 1);
			line = nl ? nl + 1 : NULL;
			continue;
		}
		char *eq = strchr(trimmed, '=');
		if (eq) {
			*eq = '\0';
			char *key = trim(trimmed);
			char *raw_val = trim(eq + 1);
			char *comment = NULL;
			int in_q = 0;
			for (char *p = raw_val; *p; p++) {
				if (*p == '"')
					in_q = !in_q;
				else if (*p == '#' && !in_q) {
					comment = p;
					break;
				}
			}
			if (comment)
				*comment = '\0';
			char *val = unquote(trim(raw_val));
			apply_kv(cfg, section, key, val);
		}
		line = nl ? nl + 1 : NULL;
	}

	free(data);
	log_info("config loaded from: %s", path);
	return 0;
}

void config_print(const struct config *cfg)
{
	if (!cfg)
		return;
	log_info("Config:");
	log_info("  [general] default_session=%s output_dir=%s",
		 cfg->general.default_session, cfg->general.output_dir);
	log_info("  [model.text] provider=%s model=%s api_base=%s",
		 cfg->models.text.provider, cfg->models.text.model,
		 cfg->models.text.api_base);
	log_info("  [react] max_iterations=%d step_timeout=%d tool_max_retries=%d",
		 cfg->react.max_iterations, cfg->react.step_timeout_seconds,
		 cfg->react.tool_max_retries);
	log_info("  [context] threshold=%.1f target=%.1f keep=%d",
		 cfg->context.summarize_threshold_ratio,
		 cfg->context.compress_target_ratio,
		 cfg->context.keep_recent_rounds);
}