#include "config.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/file.h"
#include "util/utf8.h"
#include "tomlc17.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum schema_flags {
	SCHEMA_NUMBER = 1 << 0,
	SCHEMA_RANGE = 1 << 1,
	SCHEMA_DYNAMIC_STRINGS = 1 << 2
};

struct schema_entry {
	const char *path;
	toml_type_t type;
	toml_type_t item_type;
	int flags;
	size_t max_len;
	int max_items;
	double min_value;
	double max_value;
	const char *values;
};

#define TABLE(p) { (p), TOML_TABLE, TOML_UNKNOWN, 0, 0, 0, 0, 0, NULL }
#define STRING(p, n) { (p), TOML_STRING, TOML_UNKNOWN, 0, (n), 0, 0, 0, NULL }
#define ENUM(p, n, v) { (p), TOML_STRING, TOML_UNKNOWN, 0, (n), 0, 0, 0, (v) }
#define BOOL(p) { (p), TOML_BOOLEAN, TOML_UNKNOWN, 0, 0, 0, 0, 0, NULL }
#define INT(p, lo, hi) { (p), TOML_INT64, TOML_UNKNOWN, SCHEMA_RANGE, 0, 0, (lo), (hi), NULL }
#define NUMBER(p, lo, hi) \
	{ (p), TOML_FP64, TOML_UNKNOWN, SCHEMA_NUMBER | SCHEMA_RANGE, \
	  0, 0, (lo), (hi), NULL }
#define STRINGS(p, count, len) { (p), TOML_ARRAY, TOML_STRING, 0, (len), (count), 0, 0, NULL }
#define TABLES(p, count) { (p), TOML_ARRAY, TOML_TABLE, 0, 0, (count), 0, 0, NULL }
#define DYNAMIC(p, count, len) \
	{ (p), TOML_TABLE, TOML_UNKNOWN, SCHEMA_DYNAMIC_STRINGS, \
	  (len), (count), 0, 0, NULL }

#define MODEL_FIELDS(name) \
	TABLE("model." name), \
	STRING("model." name ".provider", 63), \
	STRING("model." name ".adapter", 63), \
	STRING("model." name ".model", 127), \
	STRING("model." name ".api_base", 255), \
	STRING("model." name ".api_key_env", 63), \
	STRING("model." name ".api_key", 255), \
	INT("model." name ".context_limit", 0, INT_MAX), \
	INT("model." name ".max_tokens", 0, INT_MAX), \
	INT("model." name ".timeout_seconds", 0, INT_MAX), \
	INT("model." name ".retry_count", 0, 10), \
	INT("model." name ".poll_interval_seconds", 0, INT_MAX), \
	INT("model." name ".poll_timeout_seconds", 0, INT_MAX)

static const struct schema_entry schema[] = {
	TABLE("general"),
	STRING("general.default_session", 255),
	STRING("general.output_dir", PATH_MAX - 1),
	ENUM("general.log_level", 15, "debug|info|warn|error"),
	STRING("general.log_file", PATH_MAX - 1),
	TABLE("model"),
	MODEL_FIELDS("text"),
	MODEL_FIELDS("vision"),
	MODEL_FIELDS("image"),
	MODEL_FIELDS("video"),
	TABLE("credits"),
	INT("credits.daily_limit", -1, INT_MAX),
	STRING("credits.currency", 7),
	NUMBER("credits.cost_to_credit_coef", 0, 1.0e15),
	NUMBER("credits.input_token_credit_coef", 0, 1.0e15),
	NUMBER("credits.output_token_credit_coef", 0, 1.0e15),
	NUMBER("credits.image_unit_credit_coef", 0, 1.0e15),
	NUMBER("credits.video_second_credit_coef", 0, 1.0e15),
	TABLES("credits.prices", CREDIT_PRICE_MAX),
	STRING("credits.prices[].provider", 63),
	STRING("credits.prices[].model", 127),
	STRING("credits.prices[].kind", CREDIT_KIND_MAX - 1),
	NUMBER("credits.prices[].input_per_million", 0, 1.0e15),
	NUMBER("credits.prices[].cached_input_per_million", 0, 1.0e15),
	NUMBER("credits.prices[].output_per_million", 0, 1.0e15),
	NUMBER("credits.prices[].image_unit_per_million", 0, 1.0e15),
	NUMBER("credits.prices[].video_second_per_million", 0, 1.0e15),
	TABLE("react"),
	INT("react.max_iterations", 1, INT_MAX),
	INT("react.tool_timeout_seconds", 1, INT_MAX),
	INT("react.tool_max_retries", 0, 10),
	BOOL("react.guardrail_enabled"),
	INT("react.guardrail_max_retries", 0, 10),
	INT("react.guardrail_max_empty_rounds", 0, INT_MAX),
	STRINGS("react.guardrail_disabled_rules", GUARDRAIL_DISABLED_RULES_MAX,
		CFG_GR_NAME_MAX - 1),
	TABLES("react.guardrail_llm_rules", GUARDRAIL_LLM_RULES_MAX),
	STRING("react.guardrail_llm_rules[].name", CFG_GR_NAME_MAX - 1),
	ENUM("react.guardrail_llm_rules[].hook", 31, "input|output|tool_output"),
	STRING("react.guardrail_llm_rules[].description", CFG_GR_DESC_MAX - 1),
	STRING("react.guardrail_llm_rules[].action_text", CFG_GR_ACTION_MAX - 1),
	TABLES("react.guardrail_ext_rules", GUARDRAIL_EXT_RULES_MAX),
	STRING("react.guardrail_ext_rules[].name", CFG_GR_NAME_MAX - 1),
	ENUM("react.guardrail_ext_rules[].hook", 31, "input|output|tool_output"),
	ENUM("react.guardrail_ext_rules[].ext_type", 7, "exec|so"),
	STRING("react.guardrail_ext_rules[].ext_entry", CFG_GR_EXT_ENTRY_MAX - 1),
	STRING("react.guardrail_ext_rules[].action_text", CFG_GR_ACTION_MAX - 1),
	STRING("react.guardrail_llm_model", 127),
	STRINGS("react.disabled_tools", DISABLED_TOOLS_MAX,
		DISABLED_TOOL_NAME_MAX - 1),
	STRINGS("react.readonly_tools", READONLY_TOOLS_MAX,
		READONLY_TOOL_NAME_MAX - 1),
	BOOL("react.hitl_enabled"),
	STRINGS("react.hitl_tools", HITL_TOOLS_MAX, HITL_TOOL_NAME_MAX - 1),
	BOOL("react.hitl_auto_approve_readonly"),
	BOOL("react.bash_exec_enabled"),
	INT("react.bash_exec_default_timeout", 1, INT_MAX),
	STRINGS("react.bash_exec_allowed_commands", BASH_EXEC_ALLOW_MAX,
		BASH_EXEC_COMMAND_MAX - 1),
	STRINGS("react.bash_exec_allowed_cwds", BASH_EXEC_ALLOW_MAX,
		BASH_EXEC_CWD_MAX - 1),
	TABLE("context"),
	NUMBER("context.summarize_threshold_ratio", 0.000001, 1.0),
	NUMBER("context.compress_target_ratio", 0.000001, 1.0),
	INT("context.keep_recent_rounds", 0, INT_MAX),
	TABLE("memory"),
	BOOL("memory.enabled"),
	BOOL("memory.hot_path_enabled"),
	BOOL("memory.cold_path_enabled"),
	BOOL("memory.llm_extract_enabled"),
	INT("memory.max_facts", 0, INT_MAX),
	INT("memory.max_episodes", 0, INT_MAX),
	INT("memory.max_procedures", 0, INT_MAX),
	INT("memory.max_context_chars", 0, INT_MAX),
	TABLE("render"),
	ENUM("render.prefer_image_protocol", 15, "auto|kitty|sixel|iterm"),
	STRING("render.mpv_args", 255),
	TABLE("ext"),
	STRING("ext.dir", PATH_MAX - 1),
	INT("ext.default_max_memory_mb", 1, INT_MAX),
	INT("ext.default_max_cpu_seconds", 1, INT_MAX),
	TABLE("dynamic_tools"),
	BOOL("dynamic_tools.enabled"),
	ENUM("dynamic_tools.runtime", 15, "quickjs"),
	ENUM("dynamic_tools.mode", 15, "local|server"),
	STRING("dynamic_tools.session_dir", PATH_MAX - 1),
	STRING("dynamic_tools.persistent_dir", PATH_MAX - 1),
	ENUM("dynamic_tools.default_lifetime", 15, "session|persistent"),
	BOOL("dynamic_tools.create_requires_approval"),
	BOOL("dynamic_tools.promote_requires_approval"),
	INT("dynamic_tools.max_source_bytes", 1, INT_MAX),
	INT("dynamic_tools.default_timeout_seconds", 1, INT_MAX),
	INT("dynamic_tools.default_max_output_bytes", 1, INT_MAX),
	TABLE("dynamic_tools.local"),
	STRINGS("dynamic_tools.local.default_capabilities", DYNAMIC_TOOL_CAP_MAX,
		DYNAMIC_TOOL_CAP_LEN_MAX - 1),
	STRINGS("dynamic_tools.local.allowed_read_paths", DYNAMIC_TOOL_ALLOW_MAX,
		DYNAMIC_TOOL_ALLOW_LEN_MAX - 1),
	STRINGS("dynamic_tools.local.allowed_write_paths", DYNAMIC_TOOL_ALLOW_MAX,
		DYNAMIC_TOOL_ALLOW_LEN_MAX - 1),
	STRINGS("dynamic_tools.local.allowed_commands", DYNAMIC_TOOL_ALLOW_MAX,
		DYNAMIC_TOOL_ALLOW_LEN_MAX - 1),
	STRINGS("dynamic_tools.local.allowed_network", DYNAMIC_TOOL_ALLOW_MAX,
		DYNAMIC_TOOL_ALLOW_LEN_MAX - 1),
	TABLE("dynamic_tools.server"),
	STRINGS("dynamic_tools.server.default_capabilities", DYNAMIC_TOOL_CAP_MAX,
		DYNAMIC_TOOL_CAP_LEN_MAX - 1),
	STRINGS("dynamic_tools.server.allowed_read_paths", DYNAMIC_TOOL_ALLOW_MAX,
		DYNAMIC_TOOL_ALLOW_LEN_MAX - 1),
	STRINGS("dynamic_tools.server.allowed_write_paths", DYNAMIC_TOOL_ALLOW_MAX,
		DYNAMIC_TOOL_ALLOW_LEN_MAX - 1),
	STRINGS("dynamic_tools.server.allowed_commands", DYNAMIC_TOOL_ALLOW_MAX,
		DYNAMIC_TOOL_ALLOW_LEN_MAX - 1),
	STRINGS("dynamic_tools.server.allowed_network", DYNAMIC_TOOL_ALLOW_MAX,
		DYNAMIC_TOOL_ALLOW_LEN_MAX - 1),
	TABLE("prompt"),
	STRING("prompt.system_prompt_file", PATH_MAX - 1),
	STRING("prompt.system_prompt_dir", PATH_MAX - 1),
	TABLE("skill"),
	STRING("skill.dir", PATH_MAX - 1),
	TABLE("sync"),
	BOOL("sync.enabled"),
	STRING("sync.dir", PATH_MAX - 1),
	INT("sync.interval_seconds", 0, INT_MAX),
	INT("sync.retention_days", 0, INT_MAX),
	STRINGS("sync.include", SYNC_INCLUDE_MAX, SYNC_INCLUDE_LEN_MAX - 1),
	TABLE("mcp"),
	TABLES("mcp.servers", MCP_SERVER_MAX),
	STRING("mcp.servers[].name", MCP_SERVER_NAME_MAX - 1),
	ENUM("mcp.servers[].transport", 15, "stdio|http"),
	STRING("mcp.servers[].command", 255),
	STRINGS("mcp.servers[].args", MCP_CMD_ARGS_MAX, MCP_CMD_ARG_LEN_MAX - 1),
	DYNAMIC("mcp.servers[].env", MCP_ENV_MAX, MCP_ENV_VAL_MAX - 1),
	STRING("mcp.servers[].url", PATH_MAX - 1),
	STRING("mcp.servers[].auth_token_env", 63),
	BOOL("mcp.servers[].auto_connect"),
	INT("mcp.servers[].connect_timeout", 0, INT_MAX),
	TABLE("agent"),
	TABLES("agent.sub_agents", SUB_AGENT_MAX),
	STRING("agent.sub_agents[].name", SUB_AGENT_NAME_MAX - 1),
	STRING("agent.sub_agents[].description", 255),
	STRING("agent.sub_agents[].system_prompt_file", PATH_MAX - 1),
	STRING("agent.sub_agents[].model", 63),
	INT("agent.sub_agents[].max_iterations", 1, INT_MAX),
	STRINGS("agent.sub_agents[].allowed_tools", SUB_AGENT_TOOL_MAX,
		SUB_AGENT_TOOL_NAME_MAX - 1),
	STRINGS("agent.sub_agents[].disabled_tools", SUB_AGENT_TOOL_MAX,
		SUB_AGENT_TOOL_NAME_MAX - 1),
	ENUM("agent.sub_agents[].context_policy", 31,
		"full|summary|task_only"),
	ENUM("agent.sub_agents[].merge_strategy", 31,
		"synthesize|concat|raw"),
	STRING("agent.sub_agents[].output_schema", CONFIG_MAX_VAL_LEN)
};

static const struct schema_entry *schema_find(const char *path)
{
	for (size_t i = 0; i < sizeof(schema) / sizeof(schema[0]); i++) {
		if (strcmp(schema[i].path, path) == 0)
			return &schema[i];
	}
	return NULL;
}

static int set_error(struct config_validation_error *error,
			     enum config_validation_code code,
			     const toml_datum_t *value, const char *path,
			     const char *format, ...)
{
	va_list ap;

	if (error) {
		memset(error, 0, sizeof(*error));
		error->code = code;
		if (value) {
			error->line = value->lineno;
			error->column = value->colno;
		}
		if (path)
			strncpy(error->path, path, sizeof(error->path) - 1);
		va_start(ap, format);
		vsnprintf(error->message, sizeof(error->message), format, ap);
		va_end(ap);
	}
	return code == CONFIG_VALIDATION_SYNTAX ? MORPH_ERR_PARSE :
		MORPH_ERR_CONFIG;
}

static int value_is_allowed(const char *allowed, const char *value)
{
	const char *start;

	if (!allowed)
		return 1;
	start = allowed;
	while (*start) {
		const char *end = strchr(start, '|');
		size_t len = end ? (size_t)(end - start) : strlen(start);
		if (strlen(value) == len && strncmp(start, value, len) == 0)
			return 1;
		if (!end)
			break;
		start = end + 1;
	}
	return 0;
}

static int validate_value(const struct schema_entry *entry,
			  const toml_datum_t *value, const char *path,
			  struct config_validation_error *error)
{
	if ((entry->flags & SCHEMA_NUMBER) != 0) {
		if (value->type != TOML_FP64 && value->type != TOML_INT64)
			return set_error(error, CONFIG_VALIDATION_TYPE, value, path,
				"%s must be a number", path);
	} else if (value->type != entry->type) {
		return set_error(error, CONFIG_VALIDATION_TYPE, value, path,
			"%s has the wrong TOML type", path);
	}
	if (value->type == TOML_STRING) {
		if ((size_t)value->u.str.len > entry->max_len)
			return set_error(error, CONFIG_VALIDATION_LIMIT, value, path,
				"%s exceeds the maximum length of %zu bytes",
				path, entry->max_len);
		if (!value_is_allowed(entry->values, value->u.s))
			return set_error(error, CONFIG_VALIDATION_VALUE, value, path,
				"%s has unsupported value '%s'", path, value->u.s);
	}
	if ((entry->flags & SCHEMA_RANGE) != 0) {
		double number = value->type == TOML_INT64 ?
			(double)value->u.int64 : value->u.fp64;
		if (!isfinite(number) || number < entry->min_value ||
		    number > entry->max_value)
			return set_error(error, CONFIG_VALIDATION_RANGE, value, path,
				"%s must be between %.6g and %.6g", path,
				entry->min_value, entry->max_value);
	}
	if (value->type == TOML_ARRAY) {
		if (value->u.arr.size > entry->max_items)
			return set_error(error, CONFIG_VALIDATION_LIMIT, value, path,
				"%s has %d items; maximum is %d", path,
				value->u.arr.size, entry->max_items);
		for (int i = 0; i < value->u.arr.size; i++) {
			const toml_datum_t *item = &value->u.arr.elem[i];
			if (item->type != entry->item_type)
				return set_error(error, CONFIG_VALIDATION_TYPE, item,
					path, "%s item %d has the wrong TOML type",
					path, i);
			if (item->type == TOML_STRING &&
			    (size_t)item->u.str.len > entry->max_len)
				return set_error(error, CONFIG_VALIDATION_LIMIT, item,
					path, "%s item %d exceeds %zu bytes", path,
					i, entry->max_len);
		}
	}
	return 0;
}

static int append_path(morph_buf_t *path, const char *key)
{
	int rc;

	if (path->len > 0) {
		rc = morph_buf_putc(path, '.');
		if (rc != 0)
			return rc;
	}
	return morph_buf_puts(path, key);
}

static int validate_table(const toml_datum_t *table, morph_buf_t *path,
			  struct config_validation_error *error)
{
	for (int i = 0; i < table->u.tab.size; i++) {
		const toml_datum_t *value = &table->u.tab.value[i];
		const struct schema_entry *entry;
		size_t old_len = path->len;
		int rc = append_path(path, table->u.tab.key[i]);

		if (rc != 0)
			return rc;
		entry = schema_find(morph_buf_cstr(path));
		if (!entry) {
			return set_error(error, CONFIG_VALIDATION_UNKNOWN_KEY, value,
				morph_buf_cstr(path), "unknown configuration key: %s",
				morph_buf_cstr(path));
		}
		rc = validate_value(entry, value, morph_buf_cstr(path), error);
		if (rc != 0)
			return rc;
		if (value->type == TOML_TABLE) {
			if ((entry->flags & SCHEMA_DYNAMIC_STRINGS) != 0) {
				if (value->u.tab.size > entry->max_items)
					return set_error(error, CONFIG_VALIDATION_LIMIT,
						value, morph_buf_cstr(path),
						"%s has too many entries", morph_buf_cstr(path));
				for (int j = 0; j < value->u.tab.size; j++) {
					const toml_datum_t *child = &value->u.tab.value[j];
					if (child->type != TOML_STRING ||
					    (size_t)child->u.str.len > entry->max_len)
						return set_error(error, CONFIG_VALIDATION_TYPE,
							child, morph_buf_cstr(path),
							"%s values must be strings no longer than %zu bytes",
							morph_buf_cstr(path), entry->max_len);
				}
			} else {
				rc = validate_table(value, path, error);
				if (rc != 0)
					return rc;
			}
		} else if (value->type == TOML_ARRAY &&
			   entry->item_type == TOML_TABLE) {
			rc = morph_buf_puts(path, "[]");
			if (rc != 0)
				return rc;
			for (int j = 0; j < value->u.arr.size; j++) {
				rc = validate_table(&value->u.arr.elem[j], path, error);
				if (rc != 0)
					return rc;
			}
		}
		path->len = old_len;
		path->data[old_len] = '\0';
	}
	return 0;
}

static const toml_datum_t *table_get(const toml_datum_t *table,
				      const char *key)
{
	if (!table || table->type != TOML_TABLE)
		return NULL;
	for (int i = 0; i < table->u.tab.size; i++) {
		if (strcmp(table->u.tab.key[i], key) == 0)
			return &table->u.tab.value[i];
	}
	return NULL;
}

static int require_string(const toml_datum_t *table, const char *key,
			  const char *path, struct config_validation_error *error)
{
	const toml_datum_t *value = table_get(table, key);

	if (!value || value->type != TOML_STRING || !value->u.s[0])
		return set_error(error, CONFIG_VALIDATION_REQUIRED,
			value ? value : table, path, "%s is required", path);
	return 0;
}

static int validate_named_array(const toml_datum_t *root, const char *section,
				const char *array_name, const char *path,
				struct config_validation_error *error)
{
	const toml_datum_t *parent = table_get(root, section);
	const toml_datum_t *array = table_get(parent, array_name);

	if (!array)
		return 0;
	for (int i = 0; i < array->u.arr.size; i++) {
		const toml_datum_t *item = &array->u.arr.elem[i];
		const toml_datum_t *name = table_get(item, "name");
		int rc = require_string(item, "name", path, error);
		if (rc != 0)
			return rc;
		for (int j = 0; j < i; j++) {
			const toml_datum_t *other = table_get(&array->u.arr.elem[j],
				"name");
			if (other && strcmp(name->u.s, other->u.s) == 0)
				return set_error(error, CONFIG_VALIDATION_CONFLICT,
					name, path, "duplicate %s '%s'", path,
					name->u.s);
		}
	}
	return 0;
}

static int validate_relations(const toml_datum_t *root,
			      struct config_validation_error *error)
{
	const toml_datum_t *context = table_get(root, "context");
	const toml_datum_t *summarize = table_get(context,
		"summarize_threshold_ratio");
	const toml_datum_t *target = table_get(context, "compress_target_ratio");
	const toml_datum_t *mcp = table_get(root, "mcp");
	const toml_datum_t *servers = table_get(mcp, "servers");
	double summarize_ratio = 0.8;
	double target_ratio = 0.5;
	int rc;

	if (summarize)
		summarize_ratio = summarize->type == TOML_INT64 ?
			(double)summarize->u.int64 : summarize->u.fp64;
	if (target)
		target_ratio = target->type == TOML_INT64 ?
			(double)target->u.int64 : target->u.fp64;
	if (target_ratio >= summarize_ratio)
		return set_error(error, CONFIG_VALIDATION_CONFLICT,
			target ? target : summarize,
				"context.compress_target_ratio",
				"context.compress_target_ratio must be less than summarize_threshold_ratio");
	rc = validate_named_array(root, "mcp", "servers", "mcp.servers[].name",
		error);
	if (rc != 0)
		return rc;
	rc = validate_named_array(root, "agent", "sub_agents",
		"agent.sub_agents[].name", error);
	if (rc != 0)
		return rc;
	if (servers) {
		for (int i = 0; i < servers->u.arr.size; i++) {
			const toml_datum_t *server = &servers->u.arr.elem[i];
			const toml_datum_t *transport = table_get(server, "transport");
			rc = require_string(server, "transport",
				"mcp.servers[].transport", error);
			if (rc != 0)
				return rc;
			if (strcmp(transport->u.s, "stdio") == 0)
				rc = require_string(server, "command",
					"mcp.servers[].command", error);
			else
				rc = require_string(server, "url",
					"mcp.servers[].url", error);
			if (rc != 0)
				return rc;
		}
	}
	return 0;
}

static int parse_error_line(const char *message)
{
	int line = 0;

	if (message)
		(void)sscanf(message, "(line %d)", &line);
	return line;
}

int config_validate_text(const char *text, struct config_validation_error *error)
{
	toml_result_t parsed;
	morph_buf_t path;
	const utf8_int8_t *invalid;
	size_t text_len;
	int path_ready = 0;
	int rc;

	if (error)
		memset(error, 0, sizeof(*error));
	if (!text)
		MORPH_RETURN(-EINVAL);
	text_len = strlen(text);
	if (text_len > (size_t)INT_MAX)
		return set_error(error, CONFIG_VALIDATION_LIMIT, NULL, NULL,
			"configuration text is too large");
	invalid = utf8valid((const utf8_int8_t *)text);
	if (invalid) {
		int line = 1;
		int column = 1;
		for (const char *p = text; p < (const char *)invalid; p++) {
			if (*p == '\n') {
				line++;
				column = 1;
			} else {
				column++;
			}
		}
		rc = set_error(error, CONFIG_VALIDATION_SYNTAX, NULL, NULL,
			"configuration is not valid UTF-8");
		if (error) {
			error->line = line;
			error->column = column;
		}
		return rc;
	}
	parsed = toml_parse(text, (int)text_len);
	if (!parsed.ok) {
		rc = set_error(error, CONFIG_VALIDATION_SYNTAX, NULL, NULL,
			"%s", parsed.errmsg[0] ? parsed.errmsg : "invalid TOML");
		if (error)
			error->line = parse_error_line(parsed.errmsg);
		toml_free(parsed);
		return rc;
	}
	rc = morph_buf_init(&path, 128);
	if (rc == 0) {
		path_ready = 1;
		rc = validate_table(&parsed.toptab, &path, error);
	}
	if (rc == 0)
		rc = validate_relations(&parsed.toptab, error);
	if (path_ready)
		morph_buf_cleanup(&path);
	toml_free(parsed);
	return rc;
}

int config_validate_file(const char *path, struct config_validation_error *error)
{
	char *text;
	size_t length = 0;
	int rc;

	if (!path)
		MORPH_RETURN(-EINVAL);
	errno = 0;
	text = file_read_all(path, &length);
	if (!text)
		MORPH_RETURN(errno ? -errno : -EIO);
	(void)length;
	rc = config_validate_text(text, error);
	free(text);
	return rc;
}
