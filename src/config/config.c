#include "config.h"
#include "util/log.h"
#include "util/file.h"
#include "util/error.h"
#include "tomlc17.h"
#include "cJSON.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef toml_datum_t cfg_table_t;
typedef toml_datum_t cfg_array_t;
typedef const toml_datum_t *cfg_raw_t;

struct cfg_datum {
	int ok;
	union {
		char *s;
		int b;
		int64_t i;
		double d;
	} u;
};

typedef struct cfg_datum cfg_datum_t;

static toml_datum_t *cfg_find(cfg_table_t *table, const char *key)
{
	if (!table || table->type != TOML_TABLE || !key)
		return NULL;
	for (int i = 0; i < table->u.tab.size; i++) {
		if (strcmp(table->u.tab.key[i], key) == 0)
			return &table->u.tab.value[i];
	}
	return NULL;
}

static const toml_datum_t *cfg_find_const(const cfg_table_t *table,
					   const char *key)
{
	return cfg_find((cfg_table_t *)table, key);
}

static cfg_datum_t cfg_string_in(const cfg_table_t *table, const char *key)
{
	cfg_datum_t out = {0};
	const toml_datum_t *value = cfg_find_const(table, key);

	if (value && value->type == TOML_STRING) {
		out.u.s = strdup(value->u.s);
		out.ok = out.u.s != NULL;
	}
	return out;
}

static cfg_datum_t cfg_int_in(const cfg_table_t *table, const char *key)
{
	cfg_datum_t out = {0};
	const toml_datum_t *value = cfg_find_const(table, key);

	if (value && value->type == TOML_INT64) {
		out.ok = 1;
		out.u.i = value->u.int64;
	}
	return out;
}

static cfg_datum_t cfg_bool_in(const cfg_table_t *table, const char *key)
{
	cfg_datum_t out = {0};
	const toml_datum_t *value = cfg_find_const(table, key);

	if (value && value->type == TOML_BOOLEAN) {
		out.ok = 1;
		out.u.b = value->u.boolean ? 1 : 0;
	}
	return out;
}

static cfg_datum_t cfg_double_in(const cfg_table_t *table, const char *key)
{
	cfg_datum_t out = {0};
	const toml_datum_t *value = cfg_find_const(table, key);

	if (value && (value->type == TOML_FP64 || value->type == TOML_INT64)) {
		out.ok = 1;
		out.u.d = value->type == TOML_FP64 ? value->u.fp64 :
			(double)value->u.int64;
	}
	return out;
}

static cfg_datum_t cfg_string_at(const cfg_array_t *array, int index)
{
	cfg_datum_t out = {0};

	if (!array || array->type != TOML_ARRAY || index < 0 ||
	    index >= array->u.arr.size ||
	    array->u.arr.elem[index].type != TOML_STRING)
		return out;
	out.u.s = strdup(array->u.arr.elem[index].u.s);
	out.ok = out.u.s != NULL;
	return out;
}

static cfg_table_t *cfg_table_in(const cfg_table_t *table, const char *key)
{
	toml_datum_t *value = cfg_find((cfg_table_t *)table, key);

	return value && value->type == TOML_TABLE ? value : NULL;
}

static cfg_array_t *cfg_array_in(const cfg_table_t *table, const char *key)
{
	toml_datum_t *value = cfg_find((cfg_table_t *)table, key);

	return value && value->type == TOML_ARRAY ? value : NULL;
}

static cfg_table_t *cfg_table_at(cfg_array_t *array, int index)
{
	if (!array || array->type != TOML_ARRAY || index < 0 ||
	    index >= array->u.arr.size ||
	    array->u.arr.elem[index].type != TOML_TABLE)
		return NULL;
	return &array->u.arr.elem[index];
}

static cfg_array_t *cfg_array_at(const cfg_array_t *array, int index)
{
	if (!array || array->type != TOML_ARRAY || index < 0 ||
	    index >= array->u.arr.size ||
	    array->u.arr.elem[index].type != TOML_ARRAY)
		return NULL;
	return (cfg_array_t *)&array->u.arr.elem[index];
}

static int cfg_array_nelem(const cfg_array_t *array)
{
	return array && array->type == TOML_ARRAY ? array->u.arr.size : 0;
}

static char cfg_array_kind(const cfg_array_t *array)
{
	if (!array || array->type != TOML_ARRAY || array->u.arr.size == 0)
		return 'v';
	return array->u.arr.elem[0].type == TOML_TABLE ? 't' : 'v';
}

static const char *cfg_key_in(const cfg_table_t *table, int index)
{
	if (!table || table->type != TOML_TABLE || index < 0 ||
	    index >= table->u.tab.size)
		return NULL;
	return table->u.tab.key[index];
}

static cfg_raw_t cfg_raw_in(const cfg_table_t *table, const char *key)
{
	const toml_datum_t *value = cfg_find_const(table, key);

	if (!value || value->type == TOML_TABLE || value->type == TOML_ARRAY)
		return NULL;
	return value;
}

static cfg_raw_t cfg_raw_at(const cfg_array_t *array, int index)
{
	if (!array || array->type != TOML_ARRAY || index < 0 ||
	    index >= array->u.arr.size ||
	    array->u.arr.elem[index].type == TOML_TABLE ||
	    array->u.arr.elem[index].type == TOML_ARRAY)
		return NULL;
	return &array->u.arr.elem[index];
}

static int cfg_rtos(cfg_raw_t raw, char **out)
{
	if (!raw || raw->type != TOML_STRING || !out)
		return -EINVAL;
	*out = strdup(raw->u.s);
	return *out ? 0 : -ENOMEM;
}

static int cfg_rtob(cfg_raw_t raw, int *out)
{
	if (!raw || raw->type != TOML_BOOLEAN || !out)
		return -EINVAL;
	*out = raw->u.boolean ? 1 : 0;
	return 0;
}

static int cfg_rtoi(cfg_raw_t raw, int64_t *out)
{
	if (!raw || raw->type != TOML_INT64 || !out)
		return -EINVAL;
	*out = raw->u.int64;
	return 0;
}

static int cfg_rtod(cfg_raw_t raw, double *out)
{
	if (!raw || raw->type != TOML_FP64 || !out)
		return -EINVAL;
	*out = raw->u.fp64;
	return 0;
}

static cJSON *config_raw_json(cfg_raw_t raw, const char **kind)
{
	char *str = NULL;
	int boolean;
	int64_t integer;
	double number;

	if (cfg_rtos(raw, &str) == 0) {
		cJSON *item = cJSON_CreateString(str);
		free(str);
		*kind = "string";
		return item;
	}
	if (cfg_rtob(raw, &boolean) == 0) {
		*kind = "bool";
		return cJSON_CreateBool(boolean);
	}
	if (cfg_rtoi(raw, &integer) == 0) {
		*kind = "int";
		return cJSON_CreateNumber((double)integer);
	}
	if (cfg_rtod(raw, &number) == 0) {
		*kind = "double";
		return cJSON_CreateNumber(number);
	}
	*kind = "timestamp";
	return cJSON_CreateString("");
}

static cJSON *config_array_json(const cfg_array_t *array)
{
	cJSON *result = cJSON_CreateArray();
	int count = cfg_array_nelem(array);

	if (!result)
		return NULL;
	for (int i = 0; i < count; i++) {
		cfg_raw_t raw = cfg_raw_at(array, i);
		cJSON *item = NULL;
		if (raw) {
			const char *kind;
			item = config_raw_json(raw, &kind);
		} else if (cfg_array_at(array, i)) {
			item = config_array_json(cfg_array_at(array, i));
		}
		if (!item)
			item = cJSON_CreateNull();
		cJSON_AddItemToArray(result, item);
	}
	return result;
}

static char *config_identity_value(const cfg_table_t *table, const char *key)
{
	cfg_raw_t raw = cfg_raw_in(table, key);
	char *value = NULL;
	if (!raw || cfg_rtos(raw, &value) != 0)
		return NULL;
	return value;
}

static int config_table_identity(const char *path, const cfg_table_t *table,
				 int index, char *out, size_t out_size,
				 int *stable)
{
	char *name = NULL, *provider = NULL, *model = NULL, *kind = NULL;

	*stable = 1;
	if (strcmp(path, "mcp.servers") == 0) {
		name = config_identity_value(table, "name");
		if (name)
			snprintf(out, out_size, "name=%s", name);
		free(name);
		if (out[0])
			return 0;
	} else if (strcmp(path, "credits.prices") == 0) {
		provider = config_identity_value(table, "provider");
		model = config_identity_value(table, "model");
		kind = config_identity_value(table, "kind");
		if (provider && model && kind)
			snprintf(out, out_size, "provider=%s,model=%s,kind=%s",
				 provider, model, kind);
		free(provider);
		free(model);
		free(kind);
		if (out[0])
			return 0;
	}
	*stable = 0;
	snprintf(out, out_size, "index=%d", index);
	return 0;
}

static int config_add_entry(cJSON *entries, const char *path,
			    const char *kind, cJSON *value, int stable)
{
	cJSON *entry = cJSON_CreateObject();
	if (!entry || !value ||
	    !cJSON_AddStringToObject(entry, "path", path) ||
	    !cJSON_AddStringToObject(entry, "kind", kind) ||
	    !cJSON_AddBoolToObject(entry, "stable", stable)) {
		cJSON_Delete(entry);
		cJSON_Delete(value);
		return -ENOMEM;
	}
	cJSON_AddItemToObject(entry, "value", value);
	cJSON_AddItemToArray(entries, entry);
	return 0;
}

static int config_describe_table(const cfg_table_t *table, const char *prefix,
				 cJSON *entries, int stable)
{
	for (int i = 0;; i++) {
		const char *key = cfg_key_in(table, i);
		char path[1024];
		cfg_raw_t raw;
		cfg_array_t *array;
		cfg_table_t *child;
		int rc;

		if (!key)
			break;
		snprintf(path, sizeof(path), "%s%s%s", prefix,
			 prefix[0] ? "." : "", key);
		raw = cfg_raw_in(table, key);
		if (raw) {
			const char *kind;
			cJSON *value = config_raw_json(raw, &kind);
			rc = config_add_entry(entries, path, kind, value, stable);
			if (rc != 0)
				return rc;
			continue;
		}
		array = cfg_array_in(table, key);
		if (array) {
			if (cfg_array_kind(array) == 't') {
				for (int j = 0; j < cfg_array_nelem(array); j++) {
					cfg_table_t *item = cfg_table_at(array, j);
					char identity[512] = {0};
					char item_path[1536];
					int item_stable;
					if (!item)
						continue;
					config_table_identity(path, item, j, identity,
							      sizeof(identity), &item_stable);
					snprintf(item_path, sizeof(item_path), "%s[%s]",
						 path, identity);
					rc = config_describe_table(item, item_path, entries,
							   stable && item_stable);
					if (rc != 0)
						return rc;
				}
			} else {
				rc = config_add_entry(entries, path, "array",
						      config_array_json(array), stable);
				if (rc != 0)
					return rc;
			}
			continue;
		}
		child = cfg_table_in(table, key);
		if (child) {
			rc = config_describe_table(child, path, entries, stable);
			if (rc != 0)
				return rc;
		}
	}
	return 0;
}

char *config_describe_text(const char *text, struct config_validation_error *error)
{
	toml_result_t parsed;
	cfg_table_t *table;
	cJSON *root = NULL, *entries = NULL;
	char *json = NULL;
	int line = 0, rc;

	if (error)
		memset(error, 0, sizeof(*error));
	if (!text)
		return NULL;
	parsed = toml_parse(text, (int)strlen(text));
	if (!parsed.ok) {
		if (sscanf(parsed.errmsg, "(line %d)", &line) != 1)
			line = 0;
		if (error) {
			error->line = line;
			strncpy(error->message, parsed.errmsg[0] ? parsed.errmsg :
				"invalid TOML",
				sizeof(error->message) - 1);
		}
		toml_free(parsed);
		return NULL;
	}
	table = &parsed.toptab;
	root = cJSON_CreateObject();
	entries = cJSON_CreateArray();
	if (!root || !entries) {
		cJSON_Delete(root);
		cJSON_Delete(entries);
		toml_free(parsed);
		return NULL;
	}
	cJSON_AddItemToObject(root, "entries", entries);
	rc = config_describe_table(table, "", entries, 1);
	if (rc == 0)
		json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	toml_free(parsed);
	return json;
}

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
	cfg->models.text.max_tokens = 4096;
	cfg->models.text.timeout_seconds = 300;
	cfg->models.text.retry_count = 3;

	/* [model.vision] is optional and intentionally has no fallback. */
	cfg->models.vision.timeout_seconds = 300;
	cfg->models.vision.retry_count = 3;

	strncpy(cfg->models.image.provider, "openai",
		sizeof(cfg->models.image.provider) - 1);
	strncpy(cfg->models.image.model, "dall-e-3",
		sizeof(cfg->models.image.model) - 1);
	strncpy(cfg->models.image.api_base, "https://api.openai.com/v1",
		sizeof(cfg->models.image.api_base) - 1);
	strncpy(cfg->models.image.api_key_env, "OPENAI_API_KEY",
		sizeof(cfg->models.image.api_key_env) - 1);
	cfg->models.image.timeout_seconds = 290;

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

	cfg->credits.daily_limit = -1;
	strncpy(cfg->credits.currency, "USD",
		sizeof(cfg->credits.currency) - 1);
	cfg->credits.cost_to_credit_coef = 1000.0;
	cfg->credits.input_token_credit_coef = 0.0;
	cfg->credits.output_token_credit_coef = 0.0;
	cfg->credits.image_unit_credit_coef = 0.0;
	cfg->credits.video_second_credit_coef = 0.0;

	cfg->react.max_iterations = 10;
	cfg->react.tool_timeout_seconds = 300;
	cfg->react.tool_max_retries = 3;
	cfg->react.guardrail_enabled = 1;
	cfg->react.guardrail_max_retries = 1;
	cfg->react.guardrail_max_empty_rounds = 2;

	cfg->react.hitl_enabled = 0;
	cfg->react.hitl_tools_count = 0;
	cfg->react.hitl_auto_approve_readonly = 1;
	{
		static const char *const readonly_tools[] = {
			"file_read", "file_list", "file_info",
			"read_lines",
			"img_info", "credits", "memory",
			"ask_user", "activate_skill", "plan",
			"agent_status"
		};
		for (int i = 0; i < 11; i++) {
			strncpy(cfg->react.readonly_tools[i], readonly_tools[i],
				READONLY_TOOL_NAME_MAX - 1);
		}
		cfg->react.readonly_tools_count = 11;
	}

	cfg->react.bash_exec_enabled = 0;
	cfg->react.bash_exec_default_timeout = 60;
	strncpy(cfg->react.bash_exec_mode, "server",
		sizeof(cfg->react.bash_exec_mode) - 1);
	strncpy(cfg->react.bash_exec_server_read_paths[0], "@workdir",
		BASH_EXEC_CWD_MAX - 1);
	strncpy(cfg->react.bash_exec_server_read_paths[1], "@output",
		BASH_EXEC_CWD_MAX - 1);
	cfg->react.bash_exec_server_read_paths_count = 2;
	strncpy(cfg->react.bash_exec_server_write_paths[0], "@output",
		BASH_EXEC_CWD_MAX - 1);
	cfg->react.bash_exec_server_write_paths_count = 1;

	cfg->context.summarize_threshold_ratio = 0.8;
	cfg->context.compress_target_ratio = 0.5;
	cfg->context.keep_recent_rounds = 6;

	cfg->memory.enabled = 1;
	cfg->memory.hot_path_enabled = 1;
	cfg->memory.cold_path_enabled = 1;
	cfg->memory.llm_extract_enabled = 1;
	cfg->memory.max_facts = 6;
	cfg->memory.max_episodes = 4;
	cfg->memory.max_procedures = 4;
	cfg->memory.max_context_chars = 3000;

	strncpy(cfg->render.prefer_image_protocol, "auto",
		sizeof(cfg->render.prefer_image_protocol) - 1);
	strncpy(cfg->render.mpv_args, "--really-quiet",
		sizeof(cfg->render.mpv_args) - 1);

	strncpy(cfg->ext.dir, "~/.morph/exts",
		sizeof(cfg->ext.dir) - 1);
	cfg->ext.default_max_memory_mb = 128;
	cfg->ext.default_max_cpu_seconds = 30;

	cfg->dynamic_tools.enabled = 1;
	strncpy(cfg->dynamic_tools.runtime, "quickjs",
		sizeof(cfg->dynamic_tools.runtime) - 1);
	strncpy(cfg->dynamic_tools.mode, "local",
		sizeof(cfg->dynamic_tools.mode) - 1);
	strncpy(cfg->dynamic_tools.session_dir, "~/.morph/runtime/tools",
		sizeof(cfg->dynamic_tools.session_dir) - 1);
	strncpy(cfg->dynamic_tools.persistent_dir, "~/.morph/tools",
		sizeof(cfg->dynamic_tools.persistent_dir) - 1);
	strncpy(cfg->dynamic_tools.default_lifetime, "session",
		sizeof(cfg->dynamic_tools.default_lifetime) - 1);
	cfg->dynamic_tools.create_requires_approval = 0;
	cfg->dynamic_tools.promote_requires_approval = 1;
	cfg->dynamic_tools.max_source_bytes = 262144;
	cfg->dynamic_tools.default_timeout_seconds = 30;
	cfg->dynamic_tools.default_max_output_bytes = 1048576;
	{
		static const char *const caps[] = {
			"fs_read", "fs_write", "network", "process",
			"env", "mcp", "model", "shell", "image", "wasm"
		};
		for (int i = 0; i < 10; i++) {
			strncpy(cfg->dynamic_tools.local.default_capabilities[i],
				caps[i], DYNAMIC_TOOL_CAP_LEN_MAX - 1);
		}
		cfg->dynamic_tools.local.default_capabilities_count = 10;
		strncpy(cfg->dynamic_tools.local.allowed_read_paths[0], "*",
			DYNAMIC_TOOL_ALLOW_LEN_MAX - 1);
		strncpy(cfg->dynamic_tools.local.allowed_write_paths[0], "*",
			DYNAMIC_TOOL_ALLOW_LEN_MAX - 1);
		strncpy(cfg->dynamic_tools.local.allowed_commands[0], "*",
			DYNAMIC_TOOL_ALLOW_LEN_MAX - 1);
		strncpy(cfg->dynamic_tools.local.allowed_network[0], "*",
			DYNAMIC_TOOL_ALLOW_LEN_MAX - 1);
		cfg->dynamic_tools.local.allowed_read_paths_count = 1;
		cfg->dynamic_tools.local.allowed_write_paths_count = 1;
		cfg->dynamic_tools.local.allowed_commands_count = 1;
		cfg->dynamic_tools.local.allowed_network_count = 1;
	}

	cfg->sync.enabled = 0;
	cfg->sync.interval_seconds = 300;
	cfg->sync.retention_days = 30;
	{
		static const char *const includes[] = {
			"config.toml", "data.db", "skills", "tools", "exts", "output",
			"ui-history.db"
		};
		for (int i = 0; i < 7; i++) {
			strncpy(cfg->sync.include[i], includes[i],
				SYNC_INCLUDE_LEN_MAX - 1);
		}
		cfg->sync.include_count = 7;
	}
}

#define CFG_STR(tab, key, buf) do { \
	cfg_datum_t _d = cfg_string_in(tab, key); \
	if (_d.ok) { strncpy(buf, _d.u.s, sizeof(buf) - 1); free(_d.u.s); } \
} while (0)

#define CFG_INT(tab, key, var) do { \
	cfg_datum_t _d = cfg_int_in(tab, key); \
	if (_d.ok) var = (int)_d.u.i; \
} while (0)

#define CFG_BOOL(tab, key, var) do { \
	cfg_datum_t _d = cfg_bool_in(tab, key); \
	if (_d.ok) var = (int)_d.u.b; \
} while (0)

#define CFG_DBL(tab, key, var) do { \
	cfg_datum_t _d = cfg_double_in(tab, key); \
	if (_d.ok) var = _d.u.d; \
} while (0)

static cfg_table_t *table_path(cfg_table_t *root, const char *path)
{
	if (!root || !path || !*path)
		return root;
	char buf[256];
	strncpy(buf, path, sizeof(buf) - 1);
	cfg_table_t *tbl = root;
	char *part = buf;
	while (part && *part) {
		char *dot = strchr(part, '.');
		if (dot)
			*dot = '\0';
		if (*part) {
			tbl = cfg_table_in(tbl, part);
			if (!tbl)
				return NULL;
		}
		part = dot ? dot + 1 : NULL;
	}
	return tbl;
}

static void load_model_entry(cfg_table_t *parent, const char *sub,
			     struct config_model_entry *e)
{
	cfg_table_t *t = parent ? cfg_table_in(parent, sub) : NULL;
	if (!t)
		return;
	CFG_STR(t, "provider", e->provider);
	CFG_STR(t, "adapter", e->adapter);
	CFG_STR(t, "model", e->model);
	CFG_STR(t, "api_base", e->api_base);
	CFG_STR(t, "api_key_env", e->api_key_env);
	CFG_STR(t, "api_key", e->api_key);
	CFG_INT(t, "context_limit", e->context_limit);
	CFG_INT(t, "max_tokens", e->max_tokens);
	CFG_INT(t, "timeout_seconds", e->timeout_seconds);
	CFG_INT(t, "retry_count", e->retry_count);
	if (e->retry_count < 0)
		e->retry_count = 0;
	else if (e->retry_count > 10)
		e->retry_count = 10;
	CFG_INT(t, "poll_interval_seconds", e->poll_interval_seconds);
	CFG_INT(t, "poll_timeout_seconds", e->poll_timeout_seconds);
}

static void load_credit_price(cfg_table_t *t, struct config_credit_price *p)
{
	if (!t || !p)
		return;
	CFG_STR(t, "provider", p->provider);
	CFG_STR(t, "model", p->model);
	CFG_STR(t, "kind", p->kind);
	CFG_DBL(t, "input_per_million", p->input_per_million);
	cfg_datum_t cached_input =
		cfg_double_in(t, "cached_input_per_million");
	if (cached_input.ok) {
		p->cached_input_per_million = cached_input.u.d;
		p->cached_input_price_configured = 1;
	}
	CFG_DBL(t, "output_per_million", p->output_per_million);
	CFG_DBL(t, "image_unit_per_million", p->image_unit_per_million);
	CFG_DBL(t, "video_second_per_million", p->video_second_per_million);
}

static void load_credits_config(cfg_table_t *root, struct config_credits *cfg)
{
	cfg_table_t *credits = table_path(root, "credits");
	cfg_array_t *prices;

	if (!credits || !cfg)
		return;
	CFG_INT(credits, "daily_limit", cfg->daily_limit);
	CFG_STR(credits, "currency", cfg->currency);
	CFG_DBL(credits, "cost_to_credit_coef", cfg->cost_to_credit_coef);
	CFG_DBL(credits, "input_token_credit_coef",
		cfg->input_token_credit_coef);
	CFG_DBL(credits, "output_token_credit_coef",
		cfg->output_token_credit_coef);
	CFG_DBL(credits, "image_unit_credit_coef",
		cfg->image_unit_credit_coef);
	CFG_DBL(credits, "video_second_credit_coef",
		cfg->video_second_credit_coef);

	prices = cfg_array_in(credits, "prices");
	if (prices) {
		int count = cfg_array_nelem(prices);
		if (count > CREDIT_PRICE_MAX)
			count = CREDIT_PRICE_MAX;
		for (int i = 0; i < count; i++) {
			cfg_table_t *pt = cfg_table_at(prices, i);
			if (!pt)
				continue;
			load_credit_price(pt, &cfg->prices[cfg->price_count]);
			cfg->price_count++;
		}
	}
}

static void load_string_array(cfg_table_t *tbl, const char *key,
			      char values[][DYNAMIC_TOOL_ALLOW_LEN_MAX],
			      int *out_count, int max_count, size_t value_len)
{
	cfg_array_t *arr;
	int count;

	if (!tbl || !key || !values || !out_count)
		return;
	arr = cfg_array_in(tbl, key);
	if (!arr)
		return;
	count = 0;
	for (; count < max_count; count++) {
		cfg_datum_t val = cfg_string_at(arr, count);
		if (!val.ok)
			break;
		strncpy(values[count], val.u.s, value_len - 1);
		values[count][value_len - 1] = '\0';
		free(val.u.s);
	}
	*out_count = count;
}

static void migrate_legacy_sync_includes(struct config *cfg)
{
	static const char *const legacy[] = {
		"config.toml", "data.db", "skills", "tools", "exts", "output"
	};
	static const char *const incorrect_default[] = {
		"config.toml", "data.db", "skills", "tools", "exts", "output",
		".morph/ui-history.db"
	};

	if (!cfg)
		return;
	if (cfg->sync.include_count == 6) {
		for (int i = 0; i < 6; i++) {
			if (strcmp(cfg->sync.include[i], legacy[i]) != 0)
				return;
		}
	} else if (cfg->sync.include_count == 7) {
		for (int i = 0; i < 7; i++) {
			if (strcmp(cfg->sync.include[i], incorrect_default[i]) != 0)
				return;
		}
	} else {
		return;
	}
	strncpy(cfg->sync.include[6], "ui-history.db",
		SYNC_INCLUDE_LEN_MAX - 1);
	cfg->sync.include_count = 7;
}

static void load_cap_array(cfg_table_t *tbl, const char *key,
			   char values[][DYNAMIC_TOOL_CAP_LEN_MAX],
			   int *out_count)
{
	cfg_array_t *arr;
	int count;

	if (!tbl || !key || !values || !out_count)
		return;
	arr = cfg_array_in(tbl, key);
	if (!arr)
		return;
	count = 0;
	for (; count < DYNAMIC_TOOL_CAP_MAX; count++) {
		cfg_datum_t val = cfg_string_at(arr, count);
		if (!val.ok)
			break;
		strncpy(values[count], val.u.s, DYNAMIC_TOOL_CAP_LEN_MAX - 1);
		values[count][DYNAMIC_TOOL_CAP_LEN_MAX - 1] = '\0';
		free(val.u.s);
	}
	*out_count = count;
}

static void load_dynamic_profile(cfg_table_t *tbl,
				 struct config_dynamic_tool_profile *profile)
{
	if (!tbl || !profile)
		return;
	load_cap_array(tbl, "default_capabilities",
		       profile->default_capabilities,
		       &profile->default_capabilities_count);
	load_string_array(tbl, "allowed_read_paths",
			  profile->allowed_read_paths,
			  &profile->allowed_read_paths_count,
			  DYNAMIC_TOOL_ALLOW_MAX,
			  DYNAMIC_TOOL_ALLOW_LEN_MAX);
	load_string_array(tbl, "allowed_write_paths",
			  profile->allowed_write_paths,
			  &profile->allowed_write_paths_count,
			  DYNAMIC_TOOL_ALLOW_MAX,
			  DYNAMIC_TOOL_ALLOW_LEN_MAX);
	load_string_array(tbl, "allowed_commands",
			  profile->allowed_commands,
			  &profile->allowed_commands_count,
			  DYNAMIC_TOOL_ALLOW_MAX,
			  DYNAMIC_TOOL_ALLOW_LEN_MAX);
	load_string_array(tbl, "allowed_network",
			  profile->allowed_network,
			  &profile->allowed_network_count,
			  DYNAMIC_TOOL_ALLOW_MAX,
			  DYNAMIC_TOOL_ALLOW_LEN_MAX);
}

static void expand_path_field(char *buf, size_t len)
{
	if (!buf[0])
		return;
	char *expanded = file_expand_path(buf);
	if (expanded) {
		strncpy(buf, expanded, len - 1);
		buf[len - 1] = '\0';
		free(expanded);
	}
}

static void config_expand_paths(struct config *cfg)
{
	expand_path_field(cfg->general.output_dir, sizeof(cfg->general.output_dir));
	expand_path_field(cfg->general.log_file, sizeof(cfg->general.log_file));
	expand_path_field(cfg->ext.dir, sizeof(cfg->ext.dir));
	expand_path_field(cfg->dynamic_tools.session_dir,
			  sizeof(cfg->dynamic_tools.session_dir));
	expand_path_field(cfg->dynamic_tools.persistent_dir,
			  sizeof(cfg->dynamic_tools.persistent_dir));
	expand_path_field(cfg->skill.dir, sizeof(cfg->skill.dir));
	expand_path_field(cfg->prompt.system_prompt_file, sizeof(cfg->prompt.system_prompt_file));
	expand_path_field(cfg->prompt.system_prompt_dir, sizeof(cfg->prompt.system_prompt_dir));
	expand_path_field(cfg->sync.dir, sizeof(cfg->sync.dir));
	for (int i = 0; i < cfg->mcp.server_count; i++) {
		struct config_mcp_server *s = &cfg->mcp.servers[i];
		expand_path_field(s->command, sizeof(s->command));
	}
}

static void config_log_warning(
	const struct config_validation_error *warning, void *user_data)
{
	const char *path = user_data;

	if (!warning)
		return;
	log_warn("config warning %s:%d:%d: %s; ignoring %s",
		path ? path : "<config>", warning->line, warning->column,
		warning->message, warning->path);
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
	struct config_validation_error validation = {0};
	int validation_rc = config_validate_file_with_warnings(path, &validation,
		config_log_warning, (void *)path);
	if (validation_rc != 0) {
		fclose(f);
		log_err("invalid config %s:%d:%d: %s", path,
			validation.line, validation.column, validation.message);
		return validation_rc;
	}

	toml_result_t parsed = toml_parse_file(f);
	fclose(f);

	if (!parsed.ok) {
		log_err("config parse error: %s", parsed.errmsg);
		toml_free(parsed);
		MORPH_RETURN(MORPH_ERR_PARSE);
	}
	cfg_table_t *tbl = &parsed.toptab;

	cfg_table_t *general = table_path(tbl, "general");
	if (general) {
		CFG_STR(general, "default_session", cfg->general.default_session);
		CFG_STR(general, "output_dir", cfg->general.output_dir);
		CFG_STR(general, "log_level", cfg->general.log_level);
		CFG_STR(general, "log_file", cfg->general.log_file);
	}

	cfg_table_t *model_tbl = table_path(tbl, "model");
	load_model_entry(model_tbl, "text", &cfg->models.text);
	load_model_entry(model_tbl, "vision", &cfg->models.vision);
	load_model_entry(model_tbl, "image", &cfg->models.image);
	load_model_entry(model_tbl, "video", &cfg->models.video);
	load_credits_config(tbl, &cfg->credits);

	cfg_table_t *react = table_path(tbl, "react");
	if (react) {
		CFG_INT(react, "max_iterations", cfg->react.max_iterations);
		CFG_INT(react, "tool_timeout_seconds", cfg->react.tool_timeout_seconds);
		CFG_INT(react, "tool_max_retries", cfg->react.tool_max_retries);
		CFG_BOOL(react, "guardrail_enabled", cfg->react.guardrail_enabled);
		CFG_INT(react, "guardrail_max_retries", cfg->react.guardrail_max_retries);
		CFG_INT(react, "guardrail_max_empty_rounds", cfg->react.guardrail_max_empty_rounds);
		CFG_STR(react, "guardrail_llm_model", cfg->react.guardrail_llm_model);
		cfg_array_t *gdr = cfg_array_in(react, "guardrail_disabled_rules");
		if (gdr) {
			int gcount = 0;
			for (; gcount < GUARDRAIL_DISABLED_RULES_MAX; gcount++) {
				cfg_datum_t val = cfg_string_at(gdr, gcount);
				if (!val.ok)
					break;
				strncpy(cfg->react.guardrail_disabled_rules[gcount],
					val.u.s, CFG_GR_NAME_MAX - 1);
				free(val.u.s);
			}
			cfg->react.guardrail_disabled_rule_count = gcount;
		}
		cfg_array_t *llm_arr = cfg_array_in(react, "guardrail_llm_rules");
		if (llm_arr) {
			for (int li = 0; li < cfg_array_nelem(llm_arr) &&
			     cfg->react.guardrail_llm_rule_count < GUARDRAIL_LLM_RULES_MAX; li++) {
				cfg_table_t *lt = cfg_table_at(llm_arr, li);
				if (!lt) continue;
				struct config_guardrail_llm_rule *lr =
					&cfg->react.guardrail_llm_rules[
						cfg->react.guardrail_llm_rule_count];
				CFG_STR(lt, "name", lr->name);
				CFG_STR(lt, "hook", lr->hook);
				CFG_STR(lt, "description", lr->description);
				CFG_STR(lt, "action_text", lr->action_text);
				cfg->react.guardrail_llm_rule_count++;
			}
		}
		cfg_array_t *ext_arr = cfg_array_in(react, "guardrail_ext_rules");
		if (ext_arr) {
			for (int ei = 0; ei < cfg_array_nelem(ext_arr) &&
			     cfg->react.guardrail_ext_rule_count < GUARDRAIL_EXT_RULES_MAX; ei++) {
				cfg_table_t *et = cfg_table_at(ext_arr, ei);
				if (!et) continue;
				struct config_guardrail_ext_rule *er =
					&cfg->react.guardrail_ext_rules[
						cfg->react.guardrail_ext_rule_count];
				CFG_STR(et, "name", er->name);
				CFG_STR(et, "hook", er->hook);
				CFG_STR(et, "ext_type", er->ext_type);
				CFG_STR(et, "ext_entry", er->ext_entry);
				CFG_STR(et, "action_text", er->action_text);
				cfg->react.guardrail_ext_rule_count++;
			}
		}
		cfg_array_t *dt = cfg_array_in(react, "disabled_tools");
		if (dt) {
			int count = 0;
			for (; count < DISABLED_TOOLS_MAX; count++) {
				cfg_datum_t val = cfg_string_at(dt, count);
				if (!val.ok)
					break;
				strncpy(cfg->react.disabled_tools[count], val.u.s,
					DISABLED_TOOL_NAME_MAX - 1);
				free(val.u.s);
			}
			cfg->react.disabled_tools_count = count;
		}
		cfg_array_t *rt = cfg_array_in(react, "readonly_tools");
		if (rt) {
			int count = 0;
			for (; count < READONLY_TOOLS_MAX; count++) {
				cfg_datum_t val = cfg_string_at(rt, count);
				if (!val.ok)
					break;
				strncpy(cfg->react.readonly_tools[count], val.u.s,
					READONLY_TOOL_NAME_MAX - 1);
				free(val.u.s);
			}
			cfg->react.readonly_tools_count = count;
		}
		CFG_BOOL(react, "hitl_enabled", cfg->react.hitl_enabled);
		CFG_BOOL(react, "hitl_auto_approve_readonly", cfg->react.hitl_auto_approve_readonly);
		CFG_BOOL(react, "bash_exec_enabled", cfg->react.bash_exec_enabled);
		CFG_INT(react, "bash_exec_default_timeout", cfg->react.bash_exec_default_timeout);
		CFG_STR(react, "bash_exec_mode", cfg->react.bash_exec_mode);
		cfg_array_t *bc = cfg_array_in(react, "bash_exec_allowed_commands");
		if (bc) {
			int count = 0;
			for (; count < BASH_EXEC_ALLOW_MAX; count++) {
				cfg_datum_t val = cfg_string_at(bc, count);
				if (!val.ok)
					break;
				strncpy(cfg->react.bash_exec_allowed_commands[count],
					val.u.s, BASH_EXEC_COMMAND_MAX - 1);
				free(val.u.s);
			}
			cfg->react.bash_exec_allowed_commands_count = count;
		}
		cfg_array_t *bw = cfg_array_in(react, "bash_exec_allowed_cwds");
		if (bw) {
			int count = 0;
			for (; count < BASH_EXEC_ALLOW_MAX; count++) {
				cfg_datum_t val = cfg_string_at(bw, count);
				if (!val.ok)
					break;
				strncpy(cfg->react.bash_exec_allowed_cwds[count],
					val.u.s, BASH_EXEC_CWD_MAX - 1);
				free(val.u.s);
			}
			cfg->react.bash_exec_allowed_cwds_count = count;
		}
		{
			cfg_table_t *server = cfg_table_in(react,
							 "bash_exec_server");

			if (server) {
				load_string_array(server, "read_paths",
					cfg->react.bash_exec_server_read_paths,
					&cfg->react.bash_exec_server_read_paths_count,
					BASH_EXEC_ALLOW_MAX, BASH_EXEC_CWD_MAX);
				load_string_array(server, "write_paths",
					cfg->react.bash_exec_server_write_paths,
					&cfg->react.bash_exec_server_write_paths_count,
					BASH_EXEC_ALLOW_MAX, BASH_EXEC_CWD_MAX);
				load_string_array(server, "delete_paths",
					cfg->react.bash_exec_server_delete_paths,
					&cfg->react.bash_exec_server_delete_paths_count,
					BASH_EXEC_ALLOW_MAX, BASH_EXEC_CWD_MAX);
				CFG_BOOL(server, "network_access",
					 cfg->react.bash_exec_server_network_access);
			}
		}
		cfg_array_t *ht = cfg_array_in(react, "hitl_tools");
		if (ht) {
			int count = 0;
			for (; count < HITL_TOOLS_MAX; count++) {
				cfg_datum_t val = cfg_string_at(ht, count);
				if (!val.ok)
					break;
				strncpy(cfg->react.hitl_tools[count], val.u.s,
					HITL_TOOL_NAME_MAX - 1);
				free(val.u.s);
			}
			cfg->react.hitl_tools_count = count;
		}
	}

	cfg_table_t *context = table_path(tbl, "context");
	if (context) {
		CFG_DBL(context, "summarize_threshold_ratio", cfg->context.summarize_threshold_ratio);
		CFG_DBL(context, "compress_target_ratio", cfg->context.compress_target_ratio);
		CFG_INT(context, "keep_recent_rounds", cfg->context.keep_recent_rounds);
	}

	cfg_table_t *memory = table_path(tbl, "memory");
	if (memory) {
		CFG_BOOL(memory, "enabled", cfg->memory.enabled);
		CFG_BOOL(memory, "hot_path_enabled", cfg->memory.hot_path_enabled);
		CFG_BOOL(memory, "cold_path_enabled", cfg->memory.cold_path_enabled);
		CFG_BOOL(memory, "llm_extract_enabled", cfg->memory.llm_extract_enabled);
		CFG_INT(memory, "max_facts", cfg->memory.max_facts);
		CFG_INT(memory, "max_episodes", cfg->memory.max_episodes);
		CFG_INT(memory, "max_procedures", cfg->memory.max_procedures);
		CFG_INT(memory, "max_context_chars", cfg->memory.max_context_chars);
	}

	cfg_table_t *render = table_path(tbl, "render");
	if (render) {
		CFG_STR(render, "prefer_image_protocol", cfg->render.prefer_image_protocol);
		CFG_STR(render, "mpv_args", cfg->render.mpv_args);
	}

	cfg_table_t *ext = table_path(tbl, "ext");
	if (ext) {
		CFG_STR(ext, "dir", cfg->ext.dir);
		CFG_INT(ext, "default_max_memory_mb", cfg->ext.default_max_memory_mb);
		CFG_INT(ext, "default_max_cpu_seconds", cfg->ext.default_max_cpu_seconds);
	}

	cfg_table_t *dynamic = table_path(tbl, "dynamic_tools");
	if (dynamic) {
		cfg_datum_t mode;
		CFG_BOOL(dynamic, "enabled", cfg->dynamic_tools.enabled);
		CFG_STR(dynamic, "runtime", cfg->dynamic_tools.runtime);
		mode = cfg_string_in(dynamic, "mode");
		if (mode.ok) {
			strncpy(cfg->dynamic_tools.mode, mode.u.s,
				sizeof(cfg->dynamic_tools.mode) - 1);
			cfg->dynamic_tools.mode_explicit = 1;
			free(mode.u.s);
		}
		CFG_STR(dynamic, "session_dir", cfg->dynamic_tools.session_dir);
		CFG_STR(dynamic, "persistent_dir",
			cfg->dynamic_tools.persistent_dir);
		CFG_STR(dynamic, "default_lifetime",
			cfg->dynamic_tools.default_lifetime);
		CFG_BOOL(dynamic, "create_requires_approval",
			 cfg->dynamic_tools.create_requires_approval);
		CFG_BOOL(dynamic, "promote_requires_approval",
			 cfg->dynamic_tools.promote_requires_approval);
		CFG_INT(dynamic, "max_source_bytes",
			cfg->dynamic_tools.max_source_bytes);
		CFG_INT(dynamic, "default_timeout_seconds",
			cfg->dynamic_tools.default_timeout_seconds);
		CFG_INT(dynamic, "default_max_output_bytes",
			cfg->dynamic_tools.default_max_output_bytes);
		load_dynamic_profile(cfg_table_in(dynamic, "local"),
				     &cfg->dynamic_tools.local);
		load_dynamic_profile(cfg_table_in(dynamic, "server"),
				     &cfg->dynamic_tools.server);
	}

	cfg_table_t *prompt = table_path(tbl, "prompt");
	if (prompt) {
		CFG_STR(prompt, "system_prompt_file", cfg->prompt.system_prompt_file);
		CFG_STR(prompt, "system_prompt_dir", cfg->prompt.system_prompt_dir);
	}

	cfg_table_t *skill = table_path(tbl, "skill");
	if (skill) {
		CFG_STR(skill, "dir", cfg->skill.dir);
	}

	cfg_table_t *sync = table_path(tbl, "sync");
	if (sync) {
		CFG_BOOL(sync, "enabled", cfg->sync.enabled);
		CFG_STR(sync, "dir", cfg->sync.dir);
		CFG_INT(sync, "interval_seconds", cfg->sync.interval_seconds);
		CFG_INT(sync, "retention_days", cfg->sync.retention_days);
		load_string_array(sync, "include", cfg->sync.include,
				  &cfg->sync.include_count,
				  SYNC_INCLUDE_MAX, SYNC_INCLUDE_LEN_MAX);
		migrate_legacy_sync_includes(cfg);
	}

	cfg_table_t *mcp_tbl = table_path(tbl, "mcp");
	cfg_array_t *mcp_servers = mcp_tbl
		? cfg_array_in(mcp_tbl, "servers") : NULL;
	if (mcp_servers) {
		int i = 0;
		for (; i < cfg_array_nelem(mcp_servers) && i < MCP_SERVER_MAX; i++) {
			cfg_table_t *srv = cfg_table_at(mcp_servers, i);
			if (!srv)
				continue;
			struct config_mcp_server *ms = &cfg->mcp.servers[i];
			CFG_STR(srv, "name", ms->name);
			CFG_STR(srv, "transport", ms->transport);
			CFG_STR(srv, "command", ms->command);
			CFG_STR(srv, "url", ms->http_url);
			CFG_STR(srv, "auth_token_env", ms->http_auth_token_env);
			CFG_BOOL(srv, "auto_connect", ms->auto_connect);
			CFG_INT(srv, "connect_timeout", ms->connect_timeout);

			cfg_array_t *args_arr = cfg_array_in(srv, "args");
			if (args_arr) {
				int j = 0;
				for (; j < cfg_array_nelem(args_arr) && j < MCP_CMD_ARGS_MAX; j++) {
					cfg_datum_t a = cfg_string_at(args_arr, j);
					if (!a.ok)
						break;
					strncpy(ms->args[j], a.u.s, MCP_CMD_ARG_LEN_MAX - 1);
					ms->args_count++;
					free(a.u.s);
				}
			}

			cfg_table_t *env_tbl = cfg_table_in(srv, "env");
			if (env_tbl) {
				int ei = 0;
				for (int ki = 0; ki < 64 && ei < MCP_ENV_MAX; ki++) {
					const char *key = cfg_key_in(env_tbl, ki);
					if (!key)
						break;
					strncpy(ms->env_keys[ei], key, sizeof(ms->env_keys[ei]) - 1);
					CFG_STR(env_tbl, key, ms->env_vals[ei]);
					ei++;
				}
				ms->env_count = ei;
			}

			cfg->mcp.server_count++;
		}
	}

	toml_free(parsed);

	config_expand_paths(cfg);

	log_info("config loaded from: %s", path);

	return config_load_sub_agents(cfg, path);
}

static const char *sub_agent_ctx_policy_str(enum sub_agent_context_policy p)
{
	switch (p) {
	case SUB_AGENT_CTX_FULL:	return "full";
	case SUB_AGENT_CTX_SUMMARY:	return "summary";
	case SUB_AGENT_CTX_TASK_ONLY:	return "task_only";
	}
	return "task_only";
}

static enum sub_agent_context_policy parse_ctx_policy(const char *s)
{
	if (!s) return SUB_AGENT_CTX_TASK_ONLY;
	if (strcmp(s, "full") == 0) return SUB_AGENT_CTX_FULL;
	if (strcmp(s, "summary") == 0) return SUB_AGENT_CTX_SUMMARY;
	return SUB_AGENT_CTX_TASK_ONLY;
}

static enum sub_agent_merge_strategy parse_merge_strategy(const char *s)
{
	if (!s) return SUB_AGENT_MERGE_SYNTHESIZE;
	if (strcmp(s, "concat") == 0) return SUB_AGENT_MERGE_CONCAT;
	if (strcmp(s, "raw") == 0) return SUB_AGENT_MERGE_RAW;
	return SUB_AGENT_MERGE_SYNTHESIZE;
}

int config_load_sub_agents(struct config *cfg, const char *path)
{
	if (!cfg || !path)
		return -EINVAL;

	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	struct config_validation_error validation = {0};
	int validation_rc = config_validate_file(path, &validation);
	if (validation_rc != 0) {
		fclose(f);
		return validation_rc;
	}

	toml_result_t parsed = toml_parse_file(f);
	fclose(f);
	if (!parsed.ok) {
		toml_free(parsed);
		MORPH_RETURN(MORPH_ERR_PARSE);
	}
	cfg_table_t *tbl = &parsed.toptab;

	cfg_table_t *agent_tbl = table_path(tbl, "agent");
	cfg_array_t *sa_arr = agent_tbl
		? cfg_array_in(agent_tbl, "sub_agents") : NULL;
	if (sa_arr) {
		for (int i = 0; i < cfg_array_nelem(sa_arr) &&
		     cfg->sub_agents.count < SUB_AGENT_MAX; i++) {
			cfg_table_t *st = cfg_table_at(sa_arr, i);
			if (!st)
				continue;
			struct config_sub_agent *sa =
				&cfg->sub_agents.entries[cfg->sub_agents.count];
			CFG_STR(st, "name", sa->name);
			CFG_STR(st, "description", sa->description);
			CFG_STR(st, "system_prompt_file", sa->system_prompt_file);
			CFG_STR(st, "model", sa->model);
			CFG_INT(st, "max_iterations", sa->max_iterations);

			cfg_array_t *at = cfg_array_in(st, "allowed_tools");
			if (at) {
				for (int j = 0;
				     j < cfg_array_nelem(at) &&
				     sa->allowed_tools_count < SUB_AGENT_TOOL_MAX;
				     j++) {
					cfg_datum_t v = cfg_string_at(at, j);
					if (!v.ok) break;
					strncpy(sa->allowed_tools[j], v.u.s,
						SUB_AGENT_TOOL_NAME_MAX - 1);
					sa->allowed_tools_count++;
					free(v.u.s);
				}
			}

			cfg_array_t *dt = cfg_array_in(st, "disabled_tools");
			if (dt) {
				for (int j = 0;
				     j < cfg_array_nelem(dt) &&
				     sa->disabled_tools_count < SUB_AGENT_TOOL_MAX;
				     j++) {
					cfg_datum_t v = cfg_string_at(dt, j);
					if (!v.ok) break;
					strncpy(sa->disabled_tools[j], v.u.s,
						SUB_AGENT_TOOL_NAME_MAX - 1);
					sa->disabled_tools_count++;
					free(v.u.s);
				}
			}

			{
				cfg_datum_t cp = cfg_string_in(st,
					"context_policy");
				if (cp.ok) {
					sa->context_policy = parse_ctx_policy(
						cp.u.s);
					free(cp.u.s);
				} else {
					sa->context_policy =
						SUB_AGENT_CTX_TASK_ONLY;
				}
			}

			{
				cfg_datum_t ms = cfg_string_in(st,
					"merge_strategy");
				if (ms.ok) {
					sa->merge_strategy = parse_merge_strategy(
						ms.u.s);
					free(ms.u.s);
				} else {
					sa->merge_strategy =
						SUB_AGENT_MERGE_SYNTHESIZE;
				}
			}

			{
				cfg_datum_t os = cfg_string_in(st,
					"output_schema");
				if (os.ok) {
					sa->output_schema = os.u.s;
				}
			}

			cfg->sub_agents.count++;
		}
	}

	toml_free(parsed);
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
	log_info("  [model.vision] provider=%s model=%s api_base=%s",
		 cfg->models.vision.provider, cfg->models.vision.model,
		 cfg->models.vision.api_base);
	log_info("  [credits] daily_limit=%d currency=%s cost_coef=%.3f prices=%d",
		 cfg->credits.daily_limit, cfg->credits.currency,
		 cfg->credits.cost_to_credit_coef,
		 cfg->credits.price_count);
	log_info("  [react] max_iterations=%d tool_timeout=%d tool_max_retries=%d guardrail=%d/%d max_empty=%d disabled=%d readonly=%d hitl=%d hitl_readonly=%d hitl_tools=%d",
		 cfg->react.max_iterations, cfg->react.tool_timeout_seconds,
		 cfg->react.tool_max_retries,
		 cfg->react.guardrail_enabled, cfg->react.guardrail_max_retries,
		 cfg->react.guardrail_max_empty_rounds,
		 cfg->react.disabled_tools_count,
		 cfg->react.readonly_tools_count,
		 cfg->react.hitl_enabled, cfg->react.hitl_auto_approve_readonly,
		 cfg->react.hitl_tools_count);
	log_info("    bash_exec_enabled: %d timeout: %d mode: %s",
		 cfg->react.bash_exec_enabled,
		 cfg->react.bash_exec_default_timeout,
		 cfg->react.bash_exec_mode);
	log_info("    bash_exec_allowed_commands: %d allowed_cwds: %d",
		 cfg->react.bash_exec_allowed_commands_count,
		 cfg->react.bash_exec_allowed_cwds_count);
	log_info("    bash_exec_server: read=%d write=%d delete=%d network=%d",
		 cfg->react.bash_exec_server_read_paths_count,
		 cfg->react.bash_exec_server_write_paths_count,
		 cfg->react.bash_exec_server_delete_paths_count,
		 cfg->react.bash_exec_server_network_access);
	for (int i = 0; i < cfg->react.disabled_tools_count; i++)
		log_info("    disabled_tool: %s", cfg->react.disabled_tools[i]);
	for (int i = 0; i < cfg->react.readonly_tools_count; i++)
		log_info("    readonly_tool: %s", cfg->react.readonly_tools[i]);
	for (int i = 0; i < cfg->react.hitl_tools_count; i++)
		log_info("    hitl_tool: %s", cfg->react.hitl_tools[i]);
	log_info("  [context] threshold=%.1f target=%.1f keep=%d",
		 cfg->context.summarize_threshold_ratio,
		 cfg->context.compress_target_ratio,
		 cfg->context.keep_recent_rounds);
	log_info("  [memory] enabled=%d hot=%d cold=%d llm=%d facts=%d episodes=%d procedures=%d chars=%d",
		 cfg->memory.enabled,
		 cfg->memory.hot_path_enabled,
		 cfg->memory.cold_path_enabled,
		 cfg->memory.llm_extract_enabled,
		 cfg->memory.max_facts,
		 cfg->memory.max_episodes,
		 cfg->memory.max_procedures,
		 cfg->memory.max_context_chars);
	for (int i = 0; i < cfg->sub_agents.count; i++) {
		const struct config_sub_agent *sa = &cfg->sub_agents.entries[i];
		log_info("  [agent.sub_agents.%d] name=%s desc=%.60s max_iterations=%d ctx_policy=%s tools=%d",
			 i, sa->name, sa->description,
			 sa->max_iterations,
			 sub_agent_ctx_policy_str(sa->context_policy),
			 sa->allowed_tools_count);
	}
}
