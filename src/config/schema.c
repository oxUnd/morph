#include "config.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/utf8.h"
#include "nb/toml.h"
#include "cJSON.h"

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum schema_flags {
	SCHEMA_NUMBER = 1 << 0,
	SCHEMA_RANGE = 1 << 1,
	SCHEMA_DYNAMIC_STRINGS = 1 << 2
};

struct schema_entry {
	const char *path;
	nb_toml_type type;
	nb_toml_type item_type;
	int flags;
	size_t max_len;
	int max_items;
	double min_value;
	double max_value;
	const char *values;
};

#define TABLE(p) {(p), NB_TOML_TABLE, NB_TOML_NONE, 0, 0, 0, 0, 0, NULL}
#define STRING(p, n) {(p), NB_TOML_STRING, NB_TOML_NONE, 0, (n), 0, 0, 0, NULL}
#define ENUM(p, n, v) {(p), NB_TOML_STRING, NB_TOML_NONE, 0, (n), 0, 0, 0, (v)}
#define BOOL(p) {(p), NB_TOML_BOOLEAN, NB_TOML_NONE, 0, 0, 0, 0, 0, NULL}
#define INT(p, lo, hi) {(p), NB_TOML_INTEGER, NB_TOML_NONE, SCHEMA_RANGE, 0, 0, (lo), (hi), NULL}
#define NUMBER(p, lo, hi)                                                                          \
	{(p), NB_TOML_FLOAT, NB_TOML_NONE, SCHEMA_NUMBER | SCHEMA_RANGE, 0, 0, (lo), (hi), NULL}
#define STRINGS(p, count, len) {(p), NB_TOML_ARRAY, NB_TOML_STRING, 0, (len), (count), 0, 0, NULL}
#define INTEGERS(p, count) {(p), NB_TOML_ARRAY, NB_TOML_INTEGER, 0, 0, (count), 0, 0, NULL}
#define TABLES(p, count) {(p), NB_TOML_ARRAY, NB_TOML_TABLE, 0, 0, (count), 0, 0, NULL}
#define DYNAMIC(p, count, len)                                                                     \
	{(p), NB_TOML_TABLE, NB_TOML_NONE, SCHEMA_DYNAMIC_STRINGS, (len), (count), 0, 0, NULL}

#define MODEL_FIELDS(name) \
	TABLE("model." name), \
	STRING("model." name ".provider", 63), \
	STRING("model." name ".adapter", 63), \
	STRING("model." name ".model", 127), \
	STRING("model." name ".api_base", 255), \
	STRING("model." name ".api_key_env", 63), \
	STRING("model." name ".api_key", 255), \
	STRING("model." name ".extra_body_json", MORPH_MODEL_EXTRA_BODY_MAX - 1), \
	INT("model." name ".context_limit", 0, INT_MAX), \
	INT("model." name ".max_tokens", 0, INT_MAX), \
	INT("model." name ".timeout_seconds", 0, INT_MAX), \
	INT("model." name ".retry_count", 0, 10), \
	INT("model." name ".poll_interval_seconds", 0, INT_MAX), \
	INT("model." name ".poll_timeout_seconds", 0, INT_MAX)

#define HOOK_EVENT(name) \
	TABLES("hooks." name, 64), \
	STRING("hooks." name "[].matcher", 1024), \
	TABLES("hooks." name "[].hooks", 64), \
	STRING("hooks." name "[].hooks[].type", 32), \
	STRING("hooks." name "[].hooks[].command", 8191), \
	STRING("hooks." name "[].hooks[].statusMessage", 1024), \
	INT("hooks." name "[].hooks[].timeout", 1, 600), \
	INT("hooks." name "[].hooks[].additionalContextLimit", 0, 32768)

static const struct schema_entry schema[] = {
	TABLE("features"),
	BOOL("features.hooks"),
	TABLE("features.rollout_budget"),
	BOOL("features.rollout_budget.enabled"),
	INT("features.rollout_budget.limit_tokens", 1, INT_MAX),
	INTEGERS("features.rollout_budget.reminder_at_remaining_tokens", ROLLOUT_BUDGET_REMINDER_MAX),
	NUMBER("features.rollout_budget.prefill_token_weight", 0, DBL_MAX),
	NUMBER("features.rollout_budget.sampling_token_weight", 0, DBL_MAX),
	TABLE("hooks"),
	HOOK_EVENT("SessionStart"),
	HOOK_EVENT("SessionEnd"),
	HOOK_EVENT("UserPromptSubmit"),
	HOOK_EVENT("PreToolUse"),
	HOOK_EVENT("PostToolUse"),
	HOOK_EVENT("Stop"),
	HOOK_EVENT("PermissionRequest"),
	HOOK_EVENT("PreCompact"),
	HOOK_EVENT("PostCompact"),
	HOOK_EVENT("SubagentStart"),
	HOOK_EVENT("SubagentStop"),
	HOOK_EVENT("Interrupt"),
	TABLE("general"),
	STRING("general.default_session", 255),
	BOOL("general.auto_recap"),
	STRING("general.output_dir", PATH_MAX - 1),
	ENUM("general.log_level", 15, "debug|info|warn|error"),
	STRING("general.log_file", PATH_MAX - 1),
	TABLE("model"),
	MODEL_FIELDS("text"),
	MODEL_FIELDS("vision"),
	MODEL_FIELDS("image"),
	MODEL_FIELDS("video"),
	MODEL_FIELDS("approval"),
	TABLE("permissions"),
	ENUM("permissions.access", 31, "read_only|workspace|full"),
	ENUM("permissions.approval", 31, "on_request|never"),
	ENUM("permissions.reviewer", 31, "user|model"),
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
	TABLE("exec"),
	STRING("exec.shell", PATH_MAX - 1),
	INT("exec.default_timeout_ms", 0, INT_MAX),
	INT("exec.yield_time_ms", 0, INT_MAX),
	INT("exec.max_wait_ms", 1, INT_MAX),
	INT("exec.max_inline_output", 1024, INT_MAX),
	INT("exec.max_session_output", 1024, INT_MAX),
	INT("exec.kill_grace_ms", 0, INT_MAX),
	BOOL("exec.network"),
	BOOL("exec.allow_host_execution"),
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
	ENUM("react.guardrail_ext_rules[].on_error", 15, "continue|block"),
	STRING("react.guardrail_ext_rules[].name", CFG_GR_NAME_MAX - 1),
	ENUM("react.guardrail_ext_rules[].hook", 31, "input|output|tool_output"),
	ENUM("react.guardrail_ext_rules[].ext_type", 7, "exec|so"),
	STRING("react.guardrail_ext_rules[].ext_entry", CFG_GR_EXT_ENTRY_MAX - 1),
	STRING("react.guardrail_ext_rules[].action_text", CFG_GR_ACTION_MAX - 1),
	STRING("react.guardrail_llm_model", 127),
	STRINGS("react.disabled_tools", DISABLED_TOOLS_MAX, DISABLED_TOOL_NAME_MAX - 1),
	STRINGS("react.readonly_tools", READONLY_TOOLS_MAX, READONLY_TOOL_NAME_MAX - 1),
	BOOL("react.hitl_enabled"),
	STRINGS("react.hitl_tools", HITL_TOOLS_MAX, HITL_TOOL_NAME_MAX - 1),
	BOOL("react.hitl_auto_approve_readonly"),
	BOOL("react.hitl_auto_approve_media"),
	TABLE("react.permissions"),
	BOOL("react.permissions.request_tool_enabled"),
	STRING("react.permissions.active_profile", PERMISSION_PROFILE_NAME_MAX - 1),
	TABLES("react.permission_profiles", PERMISSION_PROFILE_MAX),
	STRING("react.permission_profiles[].name", PERMISSION_PROFILE_NAME_MAX - 1),
	STRINGS("react.permission_profiles[].workspace_roots", PERMISSION_PATH_MAX,
		PERMISSION_PATH_LEN - 1),
	STRINGS("react.permission_profiles[].write_paths", PERMISSION_PATH_MAX,
		PERMISSION_PATH_LEN - 1),
	STRINGS("react.permission_profiles[].delete_paths", PERMISSION_PATH_MAX,
		PERMISSION_PATH_LEN - 1),
	TABLE("context"),
	INT("context.project_doc_max_bytes", 1, INT_MAX),
	NUMBER("context.summarize_threshold_ratio", 0.000001, 1.0),
	NUMBER("context.compress_target_ratio", 0.000001, 1.0),
	BOOL("context.in_turn_compaction"),
	INT("context.protocol_reserve_tokens", 0, INT_MAX),
	INT("context.keep_recent_rounds", 0, INT_MAX),
	INT("context.tool_result_max_tokens", 1, INT_MAX),
	INT("context.compaction_user_message_tokens", 0, INT_MAX),
	INT("context.compaction_summary_max_tokens", 1, INT_MAX),
	STRING("context.compaction_prompt_file", PATH_MAX - 1),
	INT("context.compaction_warning_count", 0, INT_MAX),
	TABLE("memory"),
	BOOL("memory.enabled"),
	BOOL("memory.hot_path_enabled"),
	BOOL("memory.cold_path_enabled"),
	BOOL("memory.llm_extract_enabled"),
	BOOL("memory.capture_input_enabled"),
	BOOL("memory.save_tasks_enabled"),
	BOOL("memory.learn_from_chats_enabled"),
	INT("memory.max_facts", 0, INT_MAX),
	INT("memory.max_episodes", 0, INT_MAX),
	INT("memory.max_procedures", 0, INT_MAX),
	INT("memory.max_context_chars", 0, INT_MAX),
	INT("memory.max_past_tasks", 0, INT_MAX),
	INT("memory.max_tips", 0, INT_MAX),
	INT("memory.max_reply_chars", 0, INT_MAX),
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
	ENUM("prompt.mode", 15, "append|replace"),
	STRING("prompt.system_prompt_file", PATH_MAX - 1),
	STRING("prompt.system_prompt_dir", PATH_MAX - 1),
	TABLE("skill"),
	TABLE("setup"),
	ENUM("setup.role", 31,
	     "general|developer|photographer|photo_editor|writer|designer|video_creator|"
	     "researcher"),
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
	ENUM("mcp.servers[].protocol_version", 31,
	     "auto|2026-07-28|2025-11-25|2025-06-18|2025-03-26|2024-11-05"),
	BOOL("mcp.servers[].auto_connect"),
	INT("mcp.servers[].connect_timeout", 0, INT_MAX),
	TABLE("agents"),
	BOOL("agents.enabled"),
	INT("agents.max_threads", 1, 64),
	INT("agents.max_depth", 1, 16),
	INT("agents.task_timeout_seconds", 0, 86400),
	INT("agents.finalize_wait_seconds", 0, 3600),
	INT("agents.finalize_grace_seconds", 0, 3600),
	INT("agents.max_total_tokens", 0, INT_MAX),
	INT("agents.budget_input_weight", 1, 100),
	INT("agents.budget_output_weight", 1, 100),
	INT("agents.budget_warning_percent", 1, 99),
	INT("agents.budget_primary_reserve_percent", 0, 90),
	ENUM("agents.budget_enforcement", 15, "hard|warn"),
	ENUM("agents.child_shell", 15, "scoped|readonly"),
	ENUM("agents.multi_agent_mode", 31, "auto|explicit|proactive")};

static const struct schema_entry *schema_find(const char *path)
{
	for (size_t i = 0; i < sizeof(schema) / sizeof(schema[0]); i++) {
		if (strcmp(schema[i].path, path) == 0)
			return &schema[i];
	}
	return NULL;
}

static int set_error(struct config_validation_error *error, enum config_validation_code code,
		     const nb_toml_value *value, const char *path, const char *format, ...)
{
	va_list ap;

	if (error) {
		memset(error, 0, sizeof(*error));
		error->code = code;
		if (value) {
			error->line = nb_toml_location_of(value).line;
			error->column = nb_toml_location_of(value).column;
		}
		if (path)
			strncpy(error->path, path, sizeof(error->path) - 1);
		va_start(ap, format);
		vsnprintf(error->message, sizeof(error->message), format, ap);
		va_end(ap);
	}
	return code == CONFIG_VALIDATION_SYNTAX ? NB_ERR_PARSE : MORPH_ERR_CONFIG;
}

/* Configuration strings enter C APIs and fixed fields; escaped NUL cannot
 * represent a configuration value or key even though it is valid TOML. */
static int validate_text_values(const nb_toml_value *value, struct config_validation_error *error)
{
	if (nb_toml_type_of(value) == NB_TOML_STRING) {
		nb_str text = nb_toml_string(value);
		if (memchr(text.ptr, '\0', text.len))
			return set_error(error, CONFIG_VALIDATION_VALUE, value, NULL,
					 "configuration strings must not contain NUL");
	} else if (nb_toml_type_of(value) == NB_TOML_TABLE) {
		nb_toml_table_iter iter;
		nb_toml_table_iter_init(value, &iter);
		nb_str key;
		const nb_toml_value *child;
		while (nb_toml_table_iter_next(&iter, &key, &child)) {
			if (memchr(key.ptr, '\0', key.len))
				return set_error(error, CONFIG_VALIDATION_VALUE, child, NULL,
						 "configuration keys must not contain NUL");
			int rc = validate_text_values(child, error);
			if (rc != 0)
				return rc;
		}
	} else if (nb_toml_type_of(value) == NB_TOML_ARRAY) {
		for (size_t i = 0; i < nb_toml_array_size(value); i++) {
			int rc = validate_text_values(nb_toml_array_get(value, i), error);
			if (rc != 0)
				return rc;
		}
	}
	return NB_OK;
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

static int validate_value(const struct schema_entry *entry, const nb_toml_value *value,
			  const char *path, struct config_validation_error *error)
{
	if ((entry->flags & SCHEMA_NUMBER) != 0) {
		if (nb_toml_type_of(value) != NB_TOML_FLOAT &&
		    nb_toml_type_of(value) != NB_TOML_INTEGER)
			return set_error(error, CONFIG_VALIDATION_TYPE, value, path,
				"%s must be a number", path);
	} else if (nb_toml_type_of(value) != entry->type) {
		return set_error(error, CONFIG_VALIDATION_TYPE, value, path,
			"%s has the wrong TOML type", path);
	}
	if (nb_toml_type_of(value) == NB_TOML_STRING) {
		if ((size_t)nb_toml_string(value).len > entry->max_len)
			return set_error(error, CONFIG_VALIDATION_LIMIT, value, path,
				"%s exceeds the maximum length of %zu bytes",
				path, entry->max_len);
		if (!value_is_allowed(entry->values, nb_toml_string(value).ptr))
			return set_error(error, CONFIG_VALIDATION_VALUE, value, path,
					 "%s has unsupported value '%s'", path,
					 nb_toml_string(value).ptr);
	}
	if ((entry->flags & SCHEMA_RANGE) != 0) {
		double number = nb_toml_type_of(value) == NB_TOML_INTEGER
					? (double)nb_toml_integer(value)
					: nb_toml_float(value);
		if (!isfinite(number) || number < entry->min_value ||
		    number > entry->max_value)
			return set_error(error, CONFIG_VALIDATION_RANGE, value, path,
				"%s must be between %.6g and %.6g", path,
				entry->min_value, entry->max_value);
	}
	if (nb_toml_type_of(value) == NB_TOML_ARRAY) {
		if (nb_toml_array_size(value) > (size_t)entry->max_items)
			return set_error(error, CONFIG_VALIDATION_LIMIT, value, path,
					 "%s has %zu items; maximum is %d", path,
					 nb_toml_array_size(value), entry->max_items);
		for (size_t i = 0; i < nb_toml_array_size(value); i++) {
			const nb_toml_value *item = nb_toml_array_get(value, (size_t)(i));
			if (nb_toml_type_of(item) != entry->item_type)
				return set_error(error, CONFIG_VALIDATION_TYPE, item, path,
						 "%s item %zu has the wrong TOML type", path, i);
			if (nb_toml_type_of(item) == NB_TOML_STRING &&
			    (size_t)nb_toml_string(item).len > entry->max_len)
				return set_error(error, CONFIG_VALIDATION_LIMIT, item, path,
						 "%s item %zu exceeds %zu bytes", path, i,
						 entry->max_len);
		}
	}
	return 0;
}

static int append_path(nb_buf *path, const char *key)
{
	int rc;

	if (path->len > 0) {
		rc = (nb_buf_append_char(path, '.') ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
	}
	return (nb_buf_append_cstr(path, key) ? 0 : NB_ERR_NOMEM);
}

static int validate_table(const nb_toml_value *table, nb_buf *path,
			  struct config_validation_error *error,
			  config_validation_warning_fn warning_fn, void *warning_user_data)
{
	for (size_t i = 0; i < nb_toml_table_size(table); i++) {
		const nb_toml_value *value = nb_toml_table_value_at(table, (size_t)(i));
		const struct schema_entry *entry;
		size_t old_len = path->len;
		int rc = append_path(path, nb_toml_table_key_at(table, (size_t)(i)).ptr);

		if (rc != 0)
			return rc;
		entry = schema_find(nb_buf_cstr(path));
		if (!entry) {
			struct config_validation_error warning = {0};

			(void)set_error(&warning, CONFIG_VALIDATION_UNKNOWN_KEY, value,
					nb_buf_cstr(path), "unknown configuration key: %s",
					nb_buf_cstr(path));
			if (warning_fn)
				warning_fn(&warning, warning_user_data);
			path->len = old_len;
			path->ptr[old_len] = '\0';
			continue;
		}
		rc = validate_value(entry, value, nb_buf_cstr(path), error);
		if (rc != 0)
			return rc;
		if (nb_toml_type_of(value) == NB_TOML_TABLE) {
			if ((entry->flags & SCHEMA_DYNAMIC_STRINGS) != 0) {
				if (nb_toml_table_size(value) > (size_t)entry->max_items)
					return set_error(error, CONFIG_VALIDATION_LIMIT, value,
							 nb_buf_cstr(path),
							 "%s has too many entries",
							 nb_buf_cstr(path));
				for (size_t j = 0; j < nb_toml_table_size(value); j++) {
					const nb_toml_value *child =
						nb_toml_table_value_at(value, (size_t)(j));
					if (nb_toml_type_of(child) != NB_TOML_STRING ||
					    (size_t)nb_toml_string(child).len > entry->max_len)
						return set_error(error, CONFIG_VALIDATION_TYPE,
								 child, nb_buf_cstr(path),
								 "%s values must be strings no "
								 "longer than %zu bytes",
								 nb_buf_cstr(path), entry->max_len);
				}
			} else {
				rc = validate_table(value, path, error, warning_fn,
					warning_user_data);
				if (rc != 0)
					return rc;
			}
		} else if (nb_toml_type_of(value) == NB_TOML_ARRAY &&
			   entry->item_type == NB_TOML_TABLE) {
			rc = (nb_buf_append_cstr(path, "[]") ? 0 : NB_ERR_NOMEM);
			if (rc != 0)
				return rc;
			for (size_t j = 0; j < nb_toml_array_size(value); j++) {
				rc = validate_table(nb_toml_array_get(value, (size_t)(j)), path,
						    error, warning_fn, warning_user_data);
				if (rc != 0)
					return rc;
			}
		}
		path->len = old_len;
		path->ptr[old_len] = '\0';
	}
	return 0;
}

static const nb_toml_value *table_get(const nb_toml_value *table, const char *key)
{
	if (!table || nb_toml_type_of(table) != NB_TOML_TABLE)
		return NULL;
	for (size_t i = 0; i < nb_toml_table_size(table); i++) {
		if (strcmp(nb_toml_table_key_at(table, (size_t)(i)).ptr, key) == 0)
			return nb_toml_table_value_at(table, (size_t)(i));
	}
	return NULL;
}

static int require_string(const nb_toml_value *table, const char *key, const char *path,
			  struct config_validation_error *error)
{
	const nb_toml_value *value = table_get(table, key);

	if (!value || nb_toml_type_of(value) != NB_TOML_STRING || !nb_toml_string(value).ptr[0])
		return set_error(error, CONFIG_VALIDATION_REQUIRED,
			value ? value : table, path, "%s is required", path);
	return 0;
}

static int validate_named_array(const nb_toml_value *root, const char *section,
				const char *array_name, const char *path,
				struct config_validation_error *error)
{
	const nb_toml_value *parent = table_get(root, section);
	const nb_toml_value *array = table_get(parent, array_name);

	if (!array)
		return 0;
	for (size_t i = 0; i < nb_toml_array_size(array); i++) {
		const nb_toml_value *item = nb_toml_array_get(array, (size_t)(i));
		const nb_toml_value *name = table_get(item, "name");
		int rc = require_string(item, "name", path, error);
		if (rc != 0)
			return rc;
		for (size_t j = 0; j < i; j++) {
			const nb_toml_value *other =
				table_get(nb_toml_array_get(array, (size_t)(j)), "name");
			if (other &&
			    strcmp(nb_toml_string(name).ptr, nb_toml_string(other).ptr) == 0)
				return set_error(error, CONFIG_VALIDATION_CONFLICT, name, path,
						 "duplicate %s '%s'", path,
						 nb_toml_string(name).ptr);
		}
	}
	return 0;
}

static int validate_permission_profiles(const nb_toml_value *react,
					struct config_validation_error *error)
{
	static const char *const keys[] = {
		"workspace_roots", "write_paths", "delete_paths"
	};
	const nb_toml_value *profiles = table_get(react, "permission_profiles");
	const nb_toml_value *permissions = table_get(react, "permissions");
	const nb_toml_value *active = table_get(permissions, "active_profile");
	int active_found = !active || !nb_toml_string(active).ptr[0];

	if (!profiles)
		return active_found ? 0
				    : set_error(error, CONFIG_VALIDATION_CONFLICT, active,
						"react.permissions.active_profile",
						"active permission profile '%s' is not defined",
						nb_toml_string(active).ptr);
	for (size_t i = 0; i < nb_toml_array_size(profiles); i++) {
		const nb_toml_value *profile = nb_toml_array_get(profiles, (size_t)(i));
		const nb_toml_value *name = table_get(profile, "name");

		if (active && name &&
		    strcmp(nb_toml_string(active).ptr, nb_toml_string(name).ptr) == 0)
			active_found = 1;
		for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
			const nb_toml_value *paths = table_get(profile, keys[k]);

			if (!paths)
				continue;
			for (size_t j = 0; j < nb_toml_array_size(paths); j++) {
				const nb_toml_value *item = nb_toml_array_get(paths, (size_t)(j));
				const char *value = nb_toml_string(item).ptr;
				char path[CONFIG_MAX_KEY_LEN];

				if (nb_path_is_absolute(value) || value[0] == '~')
					continue;
				snprintf(path, sizeof(path),
					 "react.permission_profiles[].%s", keys[k]);
				return set_error(error, CONFIG_VALIDATION_VALUE, item, path,
						 "%s item %zu must be an absolute or ~ path", path,
						 j);
			}
		}
	}
	if (!active_found)
		return set_error(error, CONFIG_VALIDATION_CONFLICT, active,
				 "react.permissions.active_profile",
				 "active permission profile '%s' is not defined",
				 nb_toml_string(active).ptr);
	return 0;
}

static int validate_active_permission_directories(const nb_toml_value *root,
						  struct config_validation_error *error)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};

	static const char *const keys[] = {
		"workspace_roots", "write_paths", "delete_paths"
	};
	const nb_toml_value *react = table_get(root, "react");
	const nb_toml_value *permissions = table_get(react, "permissions");
	const nb_toml_value *active = table_get(permissions, "active_profile");
	const nb_toml_value *profiles = table_get(react, "permission_profiles");

	if (!active || !nb_toml_string(active).ptr[0] || !profiles)
		return 0;
	for (size_t i = 0; i < nb_toml_array_size(profiles); i++) {
		const nb_toml_value *profile = nb_toml_array_get(profiles, (size_t)(i));
		const nb_toml_value *name = table_get(profile, "name");

		if (!name || strcmp(nb_toml_string(active).ptr, nb_toml_string(name).ptr) != 0)
			continue;
		for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
			const nb_toml_value *paths = table_get(profile, keys[k]);

			if (!paths)
				continue;
			for (size_t j = 0; j < nb_toml_array_size(paths); j++) {
				const nb_toml_value *item = nb_toml_array_get(paths, (size_t)(j));
				char path[CONFIG_MAX_KEY_LEN];
				char *expanded =
					(nb_path_expand(&file_value_1, nb_toml_string(item).ptr,
							NULL) == NB_OK
						 ? strdup(nb_buf_cstr(&file_value_1))
						 : NULL);
				struct stat st;
				int stat_rc;
				int saved_errno;

				snprintf(path, sizeof(path),
					 "react.permission_profiles[].%s", keys[k]);
				if (!expanded)
					return NB_ERR_NOMEM;
				stat_rc = stat(expanded, &st);
				saved_errno = errno;
				if (stat_rc == 0 && S_ISDIR(st.st_mode)) {
					free(expanded);
					continue;
				}
				if (stat_rc == 0) {
					int rc = set_error(
						error, CONFIG_VALIDATION_VALUE, item, path,
						"active permission profile '%s' %s item %zu "
						"is not a directory: %s",
						nb_toml_string(active).ptr, keys[k], j, expanded);

					free(expanded);
					return rc;
				}
				{
					int rc = set_error(
						error, CONFIG_VALIDATION_VALUE, item, path,
						"cannot use active permission profile '%s': "
						"directory '%s' from %s item %zu: %s",
						nb_toml_string(active).ptr, expanded, keys[k], j,
						strerror(saved_errno));

					free(expanded);
					return rc;
				}
			}
		}
		return 0;
	}
	return 0;
}

static int extra_body_key_is_reserved(const char *key)
{
	static const char *const reserved[] = {
		"model", "messages", "tools", "tool_choice", "stream",
		"stream_options", "max_tokens"
	};

	for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
		if (strcmp(key, reserved[i]) == 0)
			return 1;
	}
	return 0;
}

static int validate_model_extra_body(const nb_toml_value *model, const char *name,
				     struct config_validation_error *error)
{
	const nb_toml_value *value = table_get(model, "extra_body_json");
	cJSON *extra;
	cJSON *item;
	char path[CONFIG_MAX_KEY_LEN];

	if (!value || !nb_toml_string(value).ptr[0])
		return 0;
	snprintf(path, sizeof(path), "model.%s.extra_body_json", name);
	extra = cJSON_ParseWithOpts(nb_toml_string(value).ptr, NULL, 1);
	if (!extra)
		return set_error(error, CONFIG_VALIDATION_VALUE, value, path,
			"%s must contain valid JSON", path);
	if (!cJSON_IsObject(extra)) {
		cJSON_Delete(extra);
		return set_error(error, CONFIG_VALIDATION_TYPE, value, path,
			"%s must contain a JSON object", path);
	}
	cJSON_ArrayForEach(item, extra) {
		if (item->string && extra_body_key_is_reserved(item->string)) {
			char key[CONFIG_MAX_KEY_LEN];

			strncpy(key, item->string, sizeof(key) - 1);
			key[sizeof(key) - 1] = '\0';
			cJSON_Delete(extra);
			return set_error(error, CONFIG_VALIDATION_CONFLICT, value,
				path, "%s contains reserved field '%s'", path, key);
		}
	}
	cJSON_Delete(extra);
	return 0;
}

static int validate_rollout_budget(const nb_toml_value *root, struct config_validation_error *error)
{
	const nb_toml_value *budget = table_get(table_get(root, "features"), "rollout_budget");
	const nb_toml_value *enabled = table_get(budget, "enabled");
	const nb_toml_value *limit = table_get(budget, "limit_tokens");
	const nb_toml_value *reminders = table_get(budget, "reminder_at_remaining_tokens");
	if (enabled && nb_toml_boolean(enabled) && !limit)
		return set_error(error, CONFIG_VALIDATION_REQUIRED, budget,
			"features.rollout_budget.limit_tokens",
			"limit_tokens is required when rollout budget is enabled");
	if (enabled && nb_toml_boolean(enabled) && !reminders)
		return set_error(error, CONFIG_VALIDATION_REQUIRED, budget,
			"features.rollout_budget.reminder_at_remaining_tokens",
			"reminder_at_remaining_tokens is required when rollout budget is enabled");
	if (reminders) {
		for (size_t i = 0; i < nb_toml_array_size(reminders); i++) {
			const nb_toml_value *value = nb_toml_array_get(reminders, (size_t)(i));
			if (nb_toml_integer(value) <= 0 || nb_toml_integer(value) > INT_MAX ||
			    (limit && nb_toml_integer(value) >= nb_toml_integer(limit)))
				return set_error(error, CONFIG_VALIDATION_RANGE, value,
					"features.rollout_budget.reminder_at_remaining_tokens",
					"reminders must be positive integers below limit_tokens");
		}
	}
	return 0;
}

static int validate_relations(const nb_toml_value *root, struct config_validation_error *error)
{
	const nb_toml_value *context = table_get(root, "context");
	const nb_toml_value *summarize = table_get(context, "summarize_threshold_ratio");
	const nb_toml_value *target = table_get(context, "compress_target_ratio");
	const nb_toml_value *mcp = table_get(root, "mcp");
	const nb_toml_value *servers = table_get(mcp, "servers");
	const nb_toml_value *react = table_get(root, "react");
	const nb_toml_value *exec = table_get(root, "exec");
	const nb_toml_value *shell = table_get(exec, "shell");
	const nb_toml_value *inline_output = table_get(exec, "max_inline_output");
	const nb_toml_value *session_output = table_get(exec, "max_session_output");
	const nb_toml_value *model = table_get(root, "model");
	static const char *const model_names[] = {
		"text", "vision", "image", "video", "approval"
	};
	double summarize_ratio = 0.8;
	double target_ratio = 0.5;
	int64_t inline_limit = 32768;
	int64_t session_limit = 1048576;
	int rc;
	rc = validate_rollout_budget(root, error);
	if (rc != 0)
		return rc;

	const nb_toml_value *legacy_agents = table_get(table_get(root, "agent"), "sub_agents");
	if (legacy_agents)
		return set_error(error, CONFIG_VALIDATION_VALUE, legacy_agents, "agent.sub_agents",
			"agent.sub_agents was removed; configure [agents] and use spawn_agent instead");
	if (shell && !nb_toml_string(shell).ptr[0])
		return set_error(error, CONFIG_VALIDATION_VALUE, shell,
			"exec.shell", "exec.shell must not be empty");
	if (inline_output)
		inline_limit = nb_toml_integer(inline_output);
	if (session_output)
		session_limit = nb_toml_integer(session_output);
	if (inline_limit > session_limit)
		return set_error(error, CONFIG_VALIDATION_CONFLICT,
			inline_output ? inline_output : session_output,
			"exec.max_inline_output",
			"exec.max_inline_output must not exceed "
			"exec.max_session_output");
	if (summarize)
		summarize_ratio = nb_toml_type_of(summarize) == NB_TOML_INTEGER
					  ? (double)nb_toml_integer(summarize)
					  : nb_toml_float(summarize);
	if (target)
		target_ratio = nb_toml_type_of(target) == NB_TOML_INTEGER
				       ? (double)nb_toml_integer(target)
				       : nb_toml_float(target);
	if (target_ratio >= summarize_ratio)
		return set_error(error, CONFIG_VALIDATION_CONFLICT,
			target ? target : summarize,
				"context.compress_target_ratio",
				"context.compress_target_ratio must be less than summarize_threshold_ratio");
	for (size_t i = 0;
	     i < sizeof(model_names) / sizeof(model_names[0]); i++) {
		const nb_toml_value *entry = table_get(model, model_names[i]);
		const nb_toml_value *output = table_get(entry, "max_tokens");
		const nb_toml_value *window = table_get(entry, "context_limit");

		if (i < 2 && output && window && nb_toml_integer(window) > 0 &&
		    nb_toml_integer(output) > nb_toml_integer(window)) {
			char path[CONFIG_MAX_KEY_LEN];

			snprintf(path, sizeof(path), "model.%s.max_tokens", model_names[i]);
			return set_error(error, CONFIG_VALIDATION_CONFLICT, output, path,
				"%s must not exceed model.%s.context_limit; "
				"input, reasoning, and answer tokens share the context window",
				path, model_names[i]);
		}
		rc = validate_model_extra_body(entry,
			model_names[i], error);
		if (rc != 0)
			return rc;
	}
	rc = validate_named_array(root, "mcp", "servers", "mcp.servers[].name",
		error);
	if (rc != 0)
		return rc;
	rc = validate_named_array(root, "react", "permission_profiles",
		"react.permission_profiles[].name", error);
	if (rc == 0)
		rc = validate_permission_profiles(react, error);
	if (rc != 0)
		return rc;
	if (servers) {
		for (size_t i = 0; i < nb_toml_array_size(servers); i++) {
			const nb_toml_value *server = nb_toml_array_get(servers, (size_t)(i));
			const nb_toml_value *transport = table_get(server, "transport");
			rc = require_string(server, "transport",
				"mcp.servers[].transport", error);
			if (rc != 0)
				return rc;
			if (strcmp(nb_toml_string(transport).ptr, "stdio") == 0)
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

static int model_explicit_thinking(const nb_toml_value *entry)
{
	const nb_toml_value *body = table_get(entry, "extra_body_json");
	cJSON *root;
	cJSON *thinking;
	cJSON *type;
	cJSON *effort;
	cJSON *kwargs;
	int enabled;

	if (!body || !nb_toml_string(body).ptr[0])
		return 0;
	root = cJSON_Parse(nb_toml_string(body).ptr);
	thinking = cJSON_GetObjectItemCaseSensitive(root, "thinking");
	type = cJSON_GetObjectItemCaseSensitive(thinking, "type");
	effort = cJSON_GetObjectItemCaseSensitive(root, "reasoning_effort");
	if (!cJSON_IsString(effort))
		effort = cJSON_GetObjectItemCaseSensitive(
			cJSON_GetObjectItemCaseSensitive(root, "reasoning"), "effort");
	kwargs = cJSON_GetObjectItemCaseSensitive(root, "chat_template_kwargs");
	enabled = (cJSON_IsString(effort) && strcmp(effort->valuestring, "none") != 0) ||
		cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(kwargs, "enable_thinking"));
	if (cJSON_IsString(type))
		enabled = strcmp(type->valuestring, "enabled") == 0 ||
			strcmp(type->valuestring, "adaptive") == 0;
	cJSON_Delete(root);
	return enabled;
}

static void warn_token_budgets(const nb_toml_value *root, config_validation_warning_fn warning_fn,
			       void *warning_user_data)
{
	const nb_toml_value *models = table_get(root, "model");
	static const char *const names[] = {"text", "vision", "approval"};

	if (!warning_fn)
		return;
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		const nb_toml_value *entry = table_get(models, names[i]);
		const nb_toml_value *output = table_get(entry, "max_tokens");
		const nb_toml_value *window = table_get(entry, "context_limit");
		int64_t budget =
			output && nb_toml_integer(output) > 0 ? nb_toml_integer(output) : 4096;
		struct config_validation_error warning = {0};
		char path[CONFIG_MAX_KEY_LEN];

		snprintf(path, sizeof(path), "model.%s.max_tokens", names[i]);
		if (window && nb_toml_integer(window) > 0 &&
		    budget >= (nb_toml_integer(window) + 1) / 2) {
			(void)set_error(&warning, CONFIG_VALIDATION_BUDGET,
				output ? output : window, path,
				"%s reserves at least half of model.%s.context_limit for output; "
				"this leaves less room for input and may trigger earlier "
				"compaction. Check the model's actual context and output limits",
				path, names[i]);
			warning_fn(&warning, warning_user_data);
		}
		/* An advisory, not a model capability limit: short replies may be intentional. */
		if (model_explicit_thinking(entry) && budget < 32768) {
			(void)set_error(&warning, CONFIG_VALIDATION_BUDGET,
				output ? output : table_get(entry, "extra_body_json"), path,
				"%s is below 32768 with thinking enabled; reasoning and the answer "
				"share this output budget. For long reasoning tasks, consider "
				"a larger budget within the model's supported limits", path);
			warning_fn(&warning, warning_user_data);
		}
	}
}

static int parse_error_line(const char *message)
{
	int line = 0;

	if (message)
		(void)sscanf(message, "TOML line %d", &line);
	return line;
}

static int config_validate_text_internal(const char *text,
			    struct config_validation_error *error,
			    config_validation_warning_fn warning_fn,
			    void *warning_user_data,
			    int validate_directories)
{
	nb_err parse_error = {0};
	nb_toml *parsed NB_CLEANUP(nb_toml_cleanup) = NULL;
	nb_buf path NB_CLEANUP(nb_buf_cleanup) = {0};
	size_t invalid_offset = 0;
	size_t text_len;
	int path_ready = 0;
	int rc;

	if (error)
		memset(error, 0, sizeof(*error));
	if (!text)
		return NB_ERR_INVAL;
	text_len = strlen(text);
	if (text_len > (size_t)INT_MAX)
		return set_error(error, CONFIG_VALIDATION_LIMIT, NULL, NULL,
			"configuration text is too large");
	if (nb_utf8_validate(nb_str_from_cstr(text), &invalid_offset) != NB_UTF8_OK) {
		int line = 1;
		int column = 1;
		for (const char *p = text; p < text + invalid_offset; p++) {
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
	parsed = nb_toml_parse(nb_str_from_parts(text, text_len), &parse_error);
	if (!parsed) {
		rc = set_error(error, CONFIG_VALIDATION_SYNTAX, NULL, NULL, "%s",
			       parse_error.msg[0] ? parse_error.msg : "invalid TOML");
		if (error)
			error->line = parse_error_line(parse_error.msg);
		nb_toml_cleanup(&parsed);
		return rc;
	}
	rc = (nb_buf_init(&path), nb_buf_reserve(&path, 128) ? 0 : NB_ERR_NOMEM);
	if (rc == 0) {
		path_ready = 1;
		rc = validate_text_values(nb_toml_root(parsed), error);
		if (rc == 0)
			rc = validate_table(nb_toml_root(parsed), &path, error, warning_fn,
					    warning_user_data);
	}
	if (rc == 0)
		rc = validate_relations(nb_toml_root(parsed), error);
	if (rc == 0 && validate_directories)
		rc = validate_active_permission_directories(nb_toml_root(parsed), error);
	if (rc == 0)
		warn_token_budgets(nb_toml_root(parsed), warning_fn, warning_user_data);
	if (path_ready)
		nb_buf_cleanup(&path);
	nb_toml_cleanup(&parsed);
	return rc;
}

int config_validate_text_with_warnings(const char *text,
			    struct config_validation_error *error,
			    config_validation_warning_fn warning_fn,
			    void *warning_user_data)
{
	return config_validate_text_internal(text, error, warning_fn,
		warning_user_data, 0);
}

int config_validate_text(const char *text, struct config_validation_error *error)
{
	return config_validate_text_with_warnings(text, error, NULL, NULL);
}

int config_validate_file_with_warnings(const char *path,
			    struct config_validation_error *error,
			    config_validation_warning_fn warning_fn,
			    void *warning_user_data)
{
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	int read_status_2 = NB_OK;
	char *file_copy_2 NB_CLEANUP(nb_charp_cleanup) = NULL;

	char *text;
	size_t length = 0;
	int rc;

	if (!path)
		return NB_ERR_INVAL;
	errno = 0;
	text = ((read_status_2 = nb_file_read_all(path, &file_value_2, 0, NULL)) == NB_OK &&
				(file_copy_2 = (char *)malloc(nb_buf_len(&file_value_2) + 1)) !=
					NULL
			? (memcpy(file_copy_2, nb_buf_cstr(&file_value_2),
				  nb_buf_len(&file_value_2) + 1),
			   *(&length) = nb_buf_len(&file_value_2), nb_charp_steal(&file_copy_2))
			: NULL);
	if (!text)
		return read_status_2 != NB_OK ? read_status_2 : NB_ERR_NOMEM;
	(void)length;
	rc = config_validate_text_internal(text, error, warning_fn,
		warning_user_data, 1);
	free(text);
	return rc;
}

int config_validate_file(const char *path, struct config_validation_error *error)
{
	return config_validate_file_with_warnings(path, error, NULL, NULL);
}
