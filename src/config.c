#include "config.h"
#include "util/log.h"
#include "toml.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void config_set_defaults(struct config *cfg)
{
	if (!cfg)
		return;
	memset(cfg, 0, sizeof(*cfg));

	strncpy(cfg->general.default_session, "default",
		sizeof(cfg->general.default_session) - 1);
	strncpy(cfg->general.output_dir, "~/.morph/output",
		sizeof(cfg->general.output_dir) - 1);
	strncpy(cfg->general.log_level, "info",
		sizeof(cfg->general.log_level) - 1);
	strncpy(cfg->general.log_file, "~/.morph/log/agent.log",
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
	cfg->models.text.timeout_seconds = 300;

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

	strncpy(cfg->skill.dir, "~/.morph/skills",
		sizeof(cfg->skill.dir) - 1);
	cfg->skill.default_max_memory_mb = 128;
	cfg->skill.default_max_cpu_seconds = 30;
}

#define CFG_STR(tab, key, buf) do { \
	toml_datum_t _d = toml_string_in(tab, key); \
	if (_d.ok) { strncpy(buf, _d.u.s, sizeof(buf) - 1); free(_d.u.s); } \
} while(0)

#define CFG_INT(tab, key, var) do { \
	toml_datum_t _d = toml_int_in(tab, key); \
	if (_d.ok) var = (int)_d.u.i; \
} while(0)

#define CFG_DBL(tab, key, var) do { \
	toml_datum_t _d = toml_double_in(tab, key); \
	if (_d.ok) var = _d.u.d; \
} while(0)

static toml_table_t *table_path(toml_table_t *root, const char *path)
{
	if (!root || !path || !*path)
		return root;
	char buf[256];
	strncpy(buf, path, sizeof(buf) - 1);
	toml_table_t *tbl = root;
	char *part = buf;
	while (part && *part) {
		char *dot = strchr(part, '.');
		if (dot)
			*dot = '\0';
		if (*part) {
			tbl = toml_table_in(tbl, part);
			if (!tbl)
				return NULL;
		}
		part = dot ? dot + 1 : NULL;
	}
	return tbl;
}

static void load_model_entry(toml_table_t *parent, const char *sub,
			     struct config_model_entry *e)
{
	toml_table_t *t = parent ? toml_table_in(parent, sub) : NULL;
	if (!t)
		return;
	CFG_STR(t, "provider", e->provider);
	CFG_STR(t, "model", e->model);
	CFG_STR(t, "api_base", e->api_base);
	CFG_STR(t, "api_key_env", e->api_key_env);
	CFG_STR(t, "api_key", e->api_key);
	CFG_INT(t, "context_limit", e->context_limit);
	CFG_INT(t, "timeout_seconds", e->timeout_seconds);
	CFG_INT(t, "poll_interval_seconds", e->poll_interval_seconds);
	CFG_INT(t, "poll_timeout_seconds", e->poll_timeout_seconds);
}

int config_load(struct config *cfg, const char *path)
{
	if (!cfg || !path)
		return -EINVAL;
	config_set_defaults(cfg);

	FILE *f = fopen(path, "r");
	if (!f) {
		log_warn("config file not found: %s, using defaults", path);
		return 0;
	}

	char errbuf[256];
	toml_table_t *tbl = toml_parse_file(f, errbuf, sizeof(errbuf));
	fclose(f);

	if (!tbl) {
		log_warn("config parse error: %s, using defaults", errbuf);
		return 0;
	}

	toml_table_t *general = table_path(tbl, "general");
	if (general) {
		CFG_STR(general, "default_session", cfg->general.default_session);
		CFG_STR(general, "output_dir", cfg->general.output_dir);
		CFG_STR(general, "log_level", cfg->general.log_level);
		CFG_STR(general, "log_file", cfg->general.log_file);
	}

	toml_table_t *model_tbl = table_path(tbl, "model");
	load_model_entry(model_tbl, "text", &cfg->models.text);
	load_model_entry(model_tbl, "image", &cfg->models.image);
	load_model_entry(model_tbl, "video", &cfg->models.video);

	toml_table_t *react = table_path(tbl, "react");
	if (react) {
		CFG_INT(react, "max_iterations", cfg->react.max_iterations);
		CFG_INT(react, "step_timeout_seconds", cfg->react.step_timeout_seconds);
		CFG_INT(react, "tool_max_retries", cfg->react.tool_max_retries);
		toml_array_t *dt = toml_array_in(react, "disabled_tools");
		if (dt) {
			int count = 0;
			for (; count < DISABLED_TOOLS_MAX; count++) {
				toml_datum_t val = toml_string_at(dt, count);
				if (!val.ok)
					break;
				strncpy(cfg->react.disabled_tools[count], val.u.s,
					DISABLED_TOOL_NAME_MAX - 1);
				free(val.u.s);
			}
			cfg->react.disabled_tools_count = count;
		}
	}

	toml_table_t *context = table_path(tbl, "context");
	if (context) {
		CFG_DBL(context, "summarize_threshold_ratio", cfg->context.summarize_threshold_ratio);
		CFG_DBL(context, "compress_target_ratio", cfg->context.compress_target_ratio);
		CFG_INT(context, "keep_recent_rounds", cfg->context.keep_recent_rounds);
	}

	toml_table_t *render = table_path(tbl, "render");
	if (render) {
		CFG_STR(render, "prefer_image_protocol", cfg->render.prefer_image_protocol);
		CFG_STR(render, "mpv_args", cfg->render.mpv_args);
	}

	toml_table_t *skill = table_path(tbl, "skill");
	if (skill) {
		CFG_STR(skill, "dir", cfg->skill.dir);
		CFG_INT(skill, "default_max_memory_mb", cfg->skill.default_max_memory_mb);
		CFG_INT(skill, "default_max_cpu_seconds", cfg->skill.default_max_cpu_seconds);
	}

	toml_table_t *prompt = table_path(tbl, "prompt");
	if (prompt) {
		CFG_STR(prompt, "system_prompt_file", cfg->prompt.system_prompt_file);
		CFG_STR(prompt, "system_prompt_dir", cfg->prompt.system_prompt_dir);
	}

	toml_free(tbl);
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
	log_info("  [react] max_iterations=%d step_timeout=%d tool_max_retries=%d disabled=%d",
		 cfg->react.max_iterations, cfg->react.step_timeout_seconds,
		 cfg->react.tool_max_retries, cfg->react.disabled_tools_count);
	for (int i = 0; i < cfg->react.disabled_tools_count; i++)
		log_info("    disabled_tool: %s", cfg->react.disabled_tools[i]);
	log_info("  [context] threshold=%.1f target=%.1f keep=%d",
		 cfg->context.summarize_threshold_ratio,
		 cfg->context.compress_target_ratio,
		 cfg->context.keep_recent_rounds);
}