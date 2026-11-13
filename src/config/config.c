#include "config.h"
#include "nb/cleanup.h"
#include "nb/log.h"
#include "nb/file.h"
#include "nb/str.h"
#include "errors.h"
#include "nb/toml.h"
#include "cJSON.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static cJSON *config_raw_json(const nb_toml_value *value, const char **kind)
{
	switch (nb_toml_type_of(value)) {
	case NB_TOML_STRING:
		*kind = "string";
		return cJSON_CreateString(nb_toml_string(value).ptr);
	case NB_TOML_BOOLEAN:
		*kind = "bool";
		return cJSON_CreateBool(nb_toml_boolean(value));
	case NB_TOML_INTEGER:
		*kind = "int";
		return cJSON_CreateNumber((double)nb_toml_integer(value));
	case NB_TOML_FLOAT:
		*kind = "double";
		return cJSON_CreateNumber(nb_toml_float(value));
	default:
		*kind = "timestamp";
		return cJSON_CreateString("");
	}
}

static cJSON *config_array_json(const nb_toml_value *array)
{
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateArray();

	if (!result)
		return NULL;
	for (size_t i = 0; i < nb_toml_array_size(array); i++) {
		const nb_toml_value *value = nb_toml_array_get(array, (size_t)i);
		cJSON *item NB_CLEANUP(cJSON_DeleteOwner) = NULL;
		const char *kind;

		if (nb_toml_type_of(value) == NB_TOML_ARRAY)
			item = config_array_json(value);
		else if (nb_toml_type_of(value) == NB_TOML_TABLE)
			item = cJSON_CreateNull();
		else
			item = config_raw_json(value, &kind);
		if (!item || !cJSON_AddItemToArray(result, item))
			return NULL;
		item = NULL;
	}
	return cJSON_StealOwner(&result);
}

static const char *config_identity_value(const nb_toml_value *table, const char *key)
{
	const nb_toml_value *value = nb_toml_table_get_cstr(table, key);
	return nb_toml_type_of(value) == NB_TOML_STRING ? nb_toml_string(value).ptr : NULL;
}

static int config_table_identity(const char *path, const nb_toml_value *table, int index,
				 nb_buf *out, int *stable)
{
	const char *name = NULL;
	const char *provider = NULL;
	const char *model = NULL;
	const char *kind = NULL;
	*stable = 1;
	if (strcmp(path, "mcp.servers") == 0) {
		name = config_identity_value(table, "name");
		if (name)
			(void)nb_buf_printf(out, "name=%s", name);
	} else if (strcmp(path, "credits.prices") == 0) {
		provider = config_identity_value(table, "provider");
		model = config_identity_value(table, "model");
		kind = config_identity_value(table, "kind");
		if (provider && model && kind)
			(void)nb_buf_printf(out, "provider=%s,model=%s,kind=%s", provider, model,
					    kind);
	}
	if (!nb_buf_ok(out))
		return nb_buf_status(out);
	if (!nb_buf_empty(out))
		return NB_OK;
	*stable = 0;
	return nb_buf_printf(out, "index=%d", index) ? NB_OK : nb_buf_status(out);
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
		return NB_ERR_NOMEM;
	}
	cJSON_AddItemToObject(entry, "value", value);
	cJSON_AddItemToArray(entries, entry);
	return 0;
}

static int config_describe_table(const nb_toml_value *table, const char *prefix, cJSON *entries,
				 int stable)
{
	for (int i = 0;; i++) {
		const char *key = nb_toml_table_key_at(table, (size_t)i).ptr;
		nb_buf path_buffer NB_CLEANUP(nb_buf_cleanup) = {0};
		const nb_toml_value *raw;
		const nb_toml_value *array;
		const nb_toml_value *child;
		int rc;

		if (!key)
			break;
		if (!nb_buf_printf(&path_buffer, "%s%s%s", prefix, prefix[0] ? "." : "", key))
			return nb_buf_status(&path_buffer);
		const char *path = nb_buf_cstr(&path_buffer);
		raw = nb_toml_table_get_cstr(table, key);
		if (nb_toml_type_of(raw) != NB_TOML_ARRAY &&
		    nb_toml_type_of(raw) != NB_TOML_TABLE) {
			const char *kind;
			cJSON *value = config_raw_json(raw, &kind);
			rc = config_add_entry(entries, path, kind, value, stable);
			if (rc != 0)
				return rc;
			continue;
		}
		array = nb_toml_table_get_cstr(table, key);
		if (nb_toml_type_of(array) == NB_TOML_ARRAY) {
			if (nb_toml_type_of(nb_toml_array_get(array, (size_t)0)) == NB_TOML_TABLE) {
				for (int j = 0; j < (int)nb_toml_array_size(array); j++) {
					const nb_toml_value *item =
						nb_toml_array_get(array, (size_t)j);
					nb_buf identity NB_CLEANUP(nb_buf_cleanup) = {0};
					nb_buf item_path NB_CLEANUP(nb_buf_cleanup) = {0};
					int item_stable;
					if (!item)
						continue;
					rc = config_table_identity(path, item, j, &identity,
								   &item_stable);
					if (rc != NB_OK)
						return rc;
					if (!nb_buf_printf(&item_path, "%s[%s]", path,
							   nb_buf_cstr(&identity)))
						return nb_buf_status(&item_path);
					rc = config_describe_table(item, nb_buf_cstr(&item_path),
								   entries, stable && item_stable);
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
		child = nb_toml_table_get_cstr(table, key);
		if (nb_toml_type_of(child) == NB_TOML_TABLE) {
			rc = config_describe_table(child, path, entries, stable);
			if (rc != 0)
				return rc;
		}
	}
	return 0;
}

char *config_describe_text_copy(const char *text, struct config_validation_error *error)
{
	nb_err parse_error = {0};
	nb_toml *parsed NB_CLEANUP(nb_toml_cleanup) = NULL;
	const nb_toml_value *table;
	cJSON *root = NULL, *entries = NULL;
	char *json = NULL;
	int line = 0, rc;

	if (error)
		memset(error, 0, sizeof(*error));
	if (!text)
		return NULL;
	parsed = nb_toml_parse(nb_str_from_cstr(text), &parse_error);
	if (!parsed) {
		if (sscanf(parse_error.msg, "TOML line %d", &line) != 1)
			line = 0;
		if (error) {
			error->line = line;
			nb_str message = nb_str_from_cstr(parse_error.msg[0] ? parse_error.msg
									     : "invalid TOML");
			(void)nb_str_copy_to(
				nb_str_from_parts(message.ptr,
						  NB_MIN(message.len, sizeof(error->message) - 1u)),
				error->message, sizeof(error->message));
		}
		nb_toml_cleanup(&parsed);
		return NULL;
	}
	table = nb_toml_root(parsed);
	root = cJSON_CreateObject();
	entries = cJSON_CreateArray();
	if (!root || !entries) {
		cJSON_Delete(root);
		cJSON_Delete(entries);
		nb_toml_cleanup(&parsed);
		return NULL;
	}
	cJSON_AddItemToObject(root, "entries", entries);
	rc = config_describe_table(table, "", entries, 1);
	if (rc == 0)
		json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	nb_toml_cleanup(&parsed);
	return json;
}

static cJSON *config_toml_json(const nb_toml_value *value)
{
	cJSON *out;

	if (!value)
		return NULL;
	switch (nb_toml_type_of(value)) {
	case NB_TOML_STRING:
		return cJSON_CreateString(nb_toml_string(value).ptr);
	case NB_TOML_INTEGER:
		return cJSON_CreateNumber((double)nb_toml_integer(value));
	case NB_TOML_FLOAT:
		return cJSON_CreateNumber(nb_toml_float(value));
	case NB_TOML_BOOLEAN:
		return cJSON_CreateBool(nb_toml_boolean(value));
	case NB_TOML_TABLE:
		out = cJSON_CreateObject();
		for (size_t i = 0; out && i < nb_toml_table_size(value); i++) {
			cJSON *child = config_toml_json(nb_toml_table_value_at(value, (size_t)(i)));
			if (!child ||
			    !cJSON_AddItemToObject(
				    out, nb_toml_table_key_at(value, (size_t)(i)).ptr, child)) {
				cJSON_Delete(child);
				cJSON_Delete(out);
				return NULL;
			}
		}
		return out;
	case NB_TOML_ARRAY:
		out = cJSON_CreateArray();
		for (size_t i = 0; out && i < nb_toml_array_size(value); i++) {
			cJSON *child = config_toml_json(nb_toml_array_get(value, (size_t)(i)));
			if (!child || !cJSON_AddItemToArray(out, child)) {
				cJSON_Delete(child);
				cJSON_Delete(out);
				return NULL;
			}
		}
		return out;
	default: return NULL;
	}
}

void config_set_defaults(struct nb_log *logger, struct config *cfg)
{
	if (!cfg)
		return;
	memset(cfg, 0, sizeof(*cfg));
	cfg->agents.enabled = 1;
	cfg->agents.max_threads = 6;
	cfg->agents.max_depth = 2;
	cfg->agents.task_timeout_seconds = 600;
	cfg->agents.finalize_wait_seconds = 60;
	cfg->agents.finalize_grace_seconds = 30;
	cfg->agents.rollout_budget.prefill_token_weight = 1.0;
	cfg->agents.rollout_budget.sampling_token_weight = 1.0;
	strcpy(cfg->agents.child_shell, "scoped");
	strcpy(cfg->agents.multi_agent_mode, "auto");
	cfg->logger = logger;
	cfg->permissions = permission_policy_default();
	cfg->hooks_enabled = 1;

	strncpy(cfg->prompt.mode, "append", sizeof(cfg->prompt.mode) - 1);
	strncpy(cfg->general.default_session, "default",
		sizeof(cfg->general.default_session) - 1);
	cfg->general.auto_recap = 1;
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
	strncpy(cfg->models.video.model, "doubao-seedance-2-0-260128",
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

	cfg->react.max_iterations = 2048;
	cfg->react.tool_timeout_seconds = 300;
	cfg->react.tool_max_retries = 3;
	strncpy(cfg->exec.shell, "/bin/bash", sizeof(cfg->exec.shell) - 1);
	cfg->exec.default_timeout_ms = 120000;
	cfg->exec.yield_time_ms = 10000;
	cfg->exec.max_wait_ms = EXEC_MAX_WAIT_MS_DEFAULT;
	cfg->exec.max_inline_output = 32768;
	cfg->exec.max_session_output = 1048576;
	cfg->exec.kill_grace_ms = 500;
	cfg->exec.network = 0;
	cfg->exec.allow_host_execution = 0;
	cfg->react.guardrail_enabled = 1;
	cfg->react.guardrail_max_retries = 2;
	cfg->react.guardrail_max_empty_rounds = 3;
	cfg->react.disabled_tools_count = 0;

	cfg->react.hitl_enabled = 1;
	cfg->react.hitl_tools_count = 0;
	cfg->react.hitl_auto_approve_readonly = 1;
	cfg->react.hitl_auto_approve_media = 0;
	{
		static const char *const readonly_tools[] = {
			"file_read", "file_list", "file_info",
			"read_lines",
			"img_info", "credits", "memory",
			"ask_user", "activate_skill", "plan",
			"update_plan"
		};
		for (int i = 0; i < 11; i++) {
			strncpy(cfg->react.readonly_tools[i], readonly_tools[i],
				READONLY_TOOL_NAME_MAX - 1);
		}
		cfg->react.readonly_tools_count = 11;
	}

	cfg->react.request_permissions_enabled = 1;

	cfg->context.project_doc_max_bytes = 32768;
	cfg->context.summarize_threshold_ratio = 0.8;
	cfg->context.compress_target_ratio = 0.5;
	cfg->context.in_turn_compaction = 1;
	cfg->context.protocol_reserve_tokens = 4096;
	cfg->context.keep_recent_rounds = 6;
	cfg->context.tool_result_max_tokens = 8000;
	cfg->context.compaction_user_message_tokens = 20000;
	cfg->context.compaction_summary_max_tokens = 6000;
	cfg->context.compaction_warning_count = 3;

	cfg->memory.enabled = 1;
	cfg->memory.capture_input_enabled = 1;
	cfg->memory.save_tasks_enabled = 1;
	cfg->memory.learn_from_chats_enabled = 1;
	cfg->memory.max_facts = 6;
	cfg->memory.max_past_tasks = 4;
	cfg->memory.max_tips = 4;
	cfg->memory.max_reply_chars = 3000;

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
			"ui-history.db", "input"
		};
		for (int i = 0; i < 8; i++) {
			strncpy(cfg->sync.include[i], includes[i],
				SYNC_INCLUDE_LEN_MAX - 1);
		}
		cfg->sync.include_count = 8;
	}
}

#define CFG_STR(tab, key, buf)                                                                     \
	do {                                                                                       \
		nb_str _value;                                                                     \
		if (nb_toml_get_string(nb_toml_table_get_cstr(tab, key), &_value))                 \
			(void)nb_str_copy_to(                                                      \
				nb_str_from_parts(_value.ptr,                                      \
						  NB_MIN(_value.len, sizeof(buf) - 1u)),           \
				buf, sizeof(buf));                                                 \
	} while (0)

#define CFG_INT(tab, key, var)                                                                     \
	do {                                                                                       \
		int64_t _value;                                                                    \
		if (nb_toml_get_integer(nb_toml_table_get_cstr(tab, key), &_value))                \
			var = (int)_value;                                                         \
	} while (0)

#define CFG_BOOL(tab, key, var)                                                                    \
	do {                                                                                       \
		bool _value;                                                                       \
		if (nb_toml_get_bool(nb_toml_table_get_cstr(tab, key), &_value))                   \
			var = _value;                                                              \
	} while (0)

#define CFG_DBL(tab, key, var)                                                                     \
	do {                                                                                       \
		const nb_toml_value *_value = nb_toml_table_get_cstr(tab, key);                    \
		if (nb_toml_type_of(_value) == NB_TOML_FLOAT)                                      \
			var = nb_toml_float(_value);                                               \
		else if (nb_toml_type_of(_value) == NB_TOML_INTEGER)                               \
			var = (double)nb_toml_integer(_value);                                     \
	} while (0)

static const nb_toml_value *table_path(const nb_toml_value *root, const char *path)
{
	if (!root || !path || !*path)
		return root;
	nb_split_iter parts;
	nb_str part;
	nb_split_init(&parts, nb_str_from_cstr(path), '.');
	nb_split_skip_empty(&parts, true);
	while (nb_split_next(&parts, &part)) {
		root = nb_toml_table_get(root, part);
		if (!root)
			return NULL;
	}
	return root;
}

static void load_model_entry(const nb_toml_value *parent, const char *sub,
			     struct config_model_entry *e)
{
	const nb_toml_value *t = parent ? nb_toml_table_get_cstr(parent, sub) : NULL;
	if (!t)
		return;
	CFG_STR(t, "provider", e->provider);
	CFG_STR(t, "adapter", e->adapter);
	CFG_STR(t, "model", e->model);
	CFG_STR(t, "api_base", e->api_base);
	CFG_STR(t, "api_key_env", e->api_key_env);
	CFG_STR(t, "api_key", e->api_key);
	CFG_STR(t, "extra_body_json", e->extra_body_json);
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

static void load_credit_price(const nb_toml_value *t, struct config_credit_price *p)
{
	if (!t || !p)
		return;
	CFG_STR(t, "provider", p->provider);
	CFG_STR(t, "model", p->model);
	CFG_STR(t, "kind", p->kind);
	CFG_DBL(t, "input_per_million", p->input_per_million);
	const nb_toml_value *cached_input = nb_toml_table_get_cstr(t, "cached_input_per_million");
	if (nb_toml_type_of(cached_input) == NB_TOML_FLOAT ||
	    nb_toml_type_of(cached_input) == NB_TOML_INTEGER) {
		p->cached_input_per_million = nb_toml_type_of(cached_input) == NB_TOML_FLOAT
						      ? nb_toml_float(cached_input)
						      : (double)nb_toml_integer(cached_input);
		p->cached_input_price_configured = 1;
	}
	CFG_DBL(t, "output_per_million", p->output_per_million);
	CFG_DBL(t, "image_unit_per_million", p->image_unit_per_million);
	CFG_DBL(t, "video_second_per_million", p->video_second_per_million);
}

static void load_credits_config(const nb_toml_value *root, struct config_credits *cfg)
{
	const nb_toml_value *credits = table_path(root, "credits");
	const nb_toml_value *prices;

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

	prices = nb_toml_table_get_cstr(credits, "prices");
	if (prices) {
		int count = (int)nb_toml_array_size(prices);
		if (count > CREDIT_PRICE_MAX)
			count = CREDIT_PRICE_MAX;
		for (int i = 0; i < count; i++) {
			const nb_toml_value *pt = nb_toml_array_get(prices, (size_t)i);
			if (!pt)
				continue;
			load_credit_price(pt, &cfg->prices[cfg->price_count]);
			cfg->price_count++;
		}
	}
}

static void load_string_array(const nb_toml_value *tbl, const char *key, void *values,
			      int *out_count, int max_count, size_t value_len)
{
	const nb_toml_value *arr;
	int count;

	if (!tbl || !key || !values || !out_count)
		return;
	arr = nb_toml_table_get_cstr(tbl, key);
	if (!arr)
		return;
	count = 0;
	for (; count < max_count; count++) {
		nb_str val;
		if (!nb_toml_get_string(nb_toml_array_get(arr, (size_t)(count)), &val))
			break;
		char *dst;
		dst = (char *)values + (size_t)count * value_len;
		strncpy(dst, val.ptr, value_len - 1);
		dst[value_len - 1] = '\0';
	}
	*out_count = count;
}

static void migrate_legacy_sync_includes(struct config *cfg)
{
	static const char *const legacy[] = {
		"config.toml", "data.db", "skills", "tools", "exts", "output"
	};
	if (!cfg || (cfg->sync.include_count != 6 && cfg->sync.include_count != 7))
		return;
	for (int i = 0; i < 6; i++) {
		if (strcmp(cfg->sync.include[i], legacy[i]) != 0)
			return;
	}
	if (cfg->sync.include_count == 7 &&
	    strcmp(cfg->sync.include[6], "ui-history.db") != 0 &&
	    strcmp(cfg->sync.include[6], ".morph/ui-history.db") != 0)
		return;
	strncpy(cfg->sync.include[6], "ui-history.db", SYNC_INCLUDE_LEN_MAX - 1);
	strncpy(cfg->sync.include[7], "input", SYNC_INCLUDE_LEN_MAX - 1);
	cfg->sync.include_count = 8;
}

static void load_cap_array(const nb_toml_value *tbl, const char *key,
			   char values[][DYNAMIC_TOOL_CAP_LEN_MAX], int *out_count)
{
	const nb_toml_value *arr;
	int count;

	if (!tbl || !key || !values || !out_count)
		return;
	arr = nb_toml_table_get_cstr(tbl, key);
	if (!arr)
		return;
	count = 0;
	for (; count < DYNAMIC_TOOL_CAP_MAX; count++) {
		nb_str val;
		if (!nb_toml_get_string(nb_toml_array_get(arr, (size_t)(count)), &val))
			break;
		strncpy(values[count], val.ptr, DYNAMIC_TOOL_CAP_LEN_MAX - 1);
		values[count][DYNAMIC_TOOL_CAP_LEN_MAX - 1] = '\0';
	}
	*out_count = count;
}

static void load_dynamic_profile(const nb_toml_value *tbl,
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
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};

	if (!buf[0])
		return;
	char *expanded = (nb_path_expand(&file_value_1, buf, NULL) == NB_OK
				  ? strdup(nb_buf_cstr(&file_value_1))
				  : NULL);
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

struct config_warning_context {
	struct nb_log *logger;
	const char *path;
};

static void config_log_warning(
	const struct config_validation_error *warning, void *user_data)
{
	const struct config_warning_context *context = user_data;
	const char *path = context->path;
	struct nb_log *logger = context->logger;

	if (!warning)
		return;
	nb_log_write(logger, NB_LOG_WARN, "config warning %s:%d:%d: %s%s", path ? path : "<config>",
		     warning->line, warning->column, warning->message,
		     warning->code == CONFIG_VALIDATION_UNKNOWN_KEY ? "; key ignored" : "");
}

int config_load(struct nb_log *logger, struct config *cfg, const char *path)
{
	nb_err parse_error = {0};
	if (!cfg || !path)
		return NB_ERR_INVAL;
	config_set_defaults(logger, cfg);

	FILE *f = fopen(path, "r");
	if (!f) {
		nb_log_write(logger, NB_LOG_WARN, "config file not found: %s, using defaults",
			     path);
		return 0;
	}
	struct config_validation_error validation = {0};
	struct config_warning_context warnings = {logger, path};
	int validation_rc = config_validate_file_with_warnings(path, &validation,
							       config_log_warning, &warnings);
	if (validation_rc != 0) {
		fclose(f);
		nb_log_write(logger, NB_LOG_ERROR, "invalid config %s:%d:%d: %s", path,
			     validation.line, validation.column, validation.message);
		return validation_rc;
	}

	nb_toml *parsed NB_CLEANUP(nb_toml_cleanup) = nb_toml_load(path, &parse_error);
	fclose(f);

	if (!parsed) {
		nb_log_write(logger, NB_LOG_ERROR, "config parse error: %s", parse_error.msg);
		nb_toml_cleanup(&parsed);
		return NB_ERR_PARSE;
	}
	const nb_toml_value *tbl = nb_toml_root(parsed);

	const nb_toml_value *features = table_path(tbl, "features");
	if (features)
		CFG_BOOL(features, "hooks", cfg->hooks_enabled);
	const nb_toml_value *hooks = table_path(tbl, "hooks");
	if (hooks) {
		cJSON *json = config_toml_json(hooks);
		char *text = json ? cJSON_PrintUnformatted(json) : NULL;
		if (!text || strlen(text) >= sizeof(cfg->hooks_json)) {
			free(text);
			cJSON_Delete(json);
			nb_toml_cleanup(&parsed);
			return MORPH_ERR_CONFIG;
		}
		memcpy(cfg->hooks_json, text, strlen(text) + 1);
		free(text);
		cJSON_Delete(json);
	}

	const nb_toml_value *general = table_path(tbl, "general");
	if (general) {
		CFG_BOOL(general, "auto_recap", cfg->general.auto_recap);
		CFG_STR(general, "default_session", cfg->general.default_session);
		CFG_STR(general, "output_dir", cfg->general.output_dir);
		CFG_STR(general, "log_level", cfg->general.log_level);
		CFG_STR(general, "log_file", cfg->general.log_file);
	}

	const nb_toml_value *model_tbl = table_path(tbl, "model");
	load_model_entry(model_tbl, "text", &cfg->models.text);
	load_model_entry(model_tbl, "vision", &cfg->models.vision);
	load_model_entry(model_tbl, "image", &cfg->models.image);
	load_model_entry(model_tbl, "video", &cfg->models.video);
	load_model_entry(model_tbl, "approval", &cfg->models.approval);
	load_credits_config(tbl, &cfg->credits);

	const nb_toml_value *exec = table_path(tbl, "exec");
	if (exec) {
		CFG_STR(exec, "shell", cfg->exec.shell);
		CFG_INT(exec, "default_timeout_ms", cfg->exec.default_timeout_ms);
		CFG_INT(exec, "yield_time_ms", cfg->exec.yield_time_ms);
		CFG_INT(exec, "max_wait_ms", cfg->exec.max_wait_ms);
		CFG_INT(exec, "max_inline_output", cfg->exec.max_inline_output);
		CFG_INT(exec, "max_session_output", cfg->exec.max_session_output);
		CFG_INT(exec, "kill_grace_ms", cfg->exec.kill_grace_ms);
		CFG_BOOL(exec, "network", cfg->exec.network);
		CFG_BOOL(exec, "allow_host_execution", cfg->exec.allow_host_execution);
	}

	const nb_toml_value *react = table_path(tbl, "react");
	if (react) {
		CFG_INT(react, "max_iterations", cfg->react.max_iterations);
		CFG_INT(react, "tool_timeout_seconds", cfg->react.tool_timeout_seconds);
		CFG_INT(react, "tool_max_retries", cfg->react.tool_max_retries);
		CFG_BOOL(react, "guardrail_enabled", cfg->react.guardrail_enabled);
		CFG_INT(react, "guardrail_max_retries", cfg->react.guardrail_max_retries);
		CFG_INT(react, "guardrail_max_empty_rounds", cfg->react.guardrail_max_empty_rounds);
		CFG_STR(react, "guardrail_llm_model", cfg->react.guardrail_llm_model);
		const nb_toml_value *gdr =
			nb_toml_table_get_cstr(react, "guardrail_disabled_rules");
		if (gdr) {
			int gcount = 0;
			for (; gcount < GUARDRAIL_DISABLED_RULES_MAX; gcount++) {
				nb_str val;
				if (!nb_toml_get_string(nb_toml_array_get(gdr, (size_t)(gcount)),
							&val))
					break;
				strncpy(cfg->react.guardrail_disabled_rules[gcount], val.ptr,
					CFG_GR_NAME_MAX - 1);
			}
			cfg->react.guardrail_disabled_rule_count = gcount;
		}
		const nb_toml_value *llm_arr = nb_toml_table_get_cstr(react, "guardrail_llm_rules");
		if (llm_arr) {
			for (int li = 0;
			     li < (int)nb_toml_array_size(llm_arr) &&
			     cfg->react.guardrail_llm_rule_count < GUARDRAIL_LLM_RULES_MAX;
			     li++) {
				const nb_toml_value *lt = nb_toml_array_get(llm_arr, (size_t)li);
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
		const nb_toml_value *ext_arr = nb_toml_table_get_cstr(react, "guardrail_ext_rules");
		if (ext_arr) {
			for (int ei = 0;
			     ei < (int)nb_toml_array_size(ext_arr) &&
			     cfg->react.guardrail_ext_rule_count < GUARDRAIL_EXT_RULES_MAX;
			     ei++) {
				const nb_toml_value *et = nb_toml_array_get(ext_arr, (size_t)ei);
				if (!et) continue;
				struct config_guardrail_ext_rule *er =
					&cfg->react.guardrail_ext_rules[
						cfg->react.guardrail_ext_rule_count];
				CFG_STR(et, "on_error", er->on_error);
				CFG_STR(et, "name", er->name);
				CFG_STR(et, "hook", er->hook);
				CFG_STR(et, "ext_type", er->ext_type);
				CFG_STR(et, "ext_entry", er->ext_entry);
				CFG_STR(et, "action_text", er->action_text);
				cfg->react.guardrail_ext_rule_count++;
			}
		}
		const nb_toml_value *dt = nb_toml_table_get_cstr(react, "disabled_tools");
		if (dt) {
			int count = 0;

			memset(cfg->react.disabled_tools, 0,
			       sizeof(cfg->react.disabled_tools));
			for (; count < DISABLED_TOOLS_MAX; count++) {
				nb_str val;
				if (!nb_toml_get_string(nb_toml_array_get(dt, (size_t)(count)),
							&val))
					break;
				strncpy(cfg->react.disabled_tools[count], val.ptr,
					DISABLED_TOOL_NAME_MAX - 1);
			}
			cfg->react.disabled_tools_count = count;
		}
		const nb_toml_value *rt = nb_toml_table_get_cstr(react, "readonly_tools");
		if (rt) {
			int count = 0;
			for (; count < READONLY_TOOLS_MAX; count++) {
				nb_str val;
				if (!nb_toml_get_string(nb_toml_array_get(rt, (size_t)(count)),
							&val))
					break;
				strncpy(cfg->react.readonly_tools[count], val.ptr,
					READONLY_TOOL_NAME_MAX - 1);
			}
			cfg->react.readonly_tools_count = count;
		}
		CFG_BOOL(react, "hitl_enabled", cfg->react.hitl_enabled);
		CFG_BOOL(react, "hitl_auto_approve_readonly", cfg->react.hitl_auto_approve_readonly);
		CFG_BOOL(react, "hitl_auto_approve_media", cfg->react.hitl_auto_approve_media);
		{
			const nb_toml_value *permissions =
				nb_toml_table_get_cstr(react, "permissions");

			if (permissions) {
				CFG_BOOL(permissions, "request_tool_enabled",
					 cfg->react.request_permissions_enabled);
				CFG_STR(permissions, "active_profile",
					cfg->react.permission_active_profile);
			}
		}
		{
			const nb_toml_value *profiles =
				nb_toml_table_get_cstr(react, "permission_profiles");

			if (profiles) {
				int count = (int)nb_toml_array_size(profiles);

				if (count > PERMISSION_PROFILE_MAX)
					count = PERMISSION_PROFILE_MAX;
				for (int i = 0; i < count; i++) {
					const nb_toml_value *profile =
						nb_toml_array_get(profiles, (size_t)i);
					struct config_permission_profile *dst =
						&cfg->react.permission_profiles[i];

					if (!profile)
						continue;
					CFG_STR(profile, "name", dst->name);
					load_string_array(profile, "workspace_roots",
						dst->workspace_roots,
						&dst->workspace_roots_count,
						PERMISSION_PATH_MAX, PERMISSION_PATH_LEN);
					load_string_array(profile, "write_paths",
						dst->write_paths, &dst->write_paths_count,
						PERMISSION_PATH_MAX, PERMISSION_PATH_LEN);
					load_string_array(profile, "delete_paths",
						dst->delete_paths, &dst->delete_paths_count,
						PERMISSION_PATH_MAX, PERMISSION_PATH_LEN);
					cfg->react.permission_profile_count++;
				}
			}
		}
		const nb_toml_value *ht = nb_toml_table_get_cstr(react, "hitl_tools");
		if (ht) {
			int count = 0;
			for (; count < HITL_TOOLS_MAX; count++) {
				nb_str val;
				if (!nb_toml_get_string(nb_toml_array_get(ht, (size_t)(count)),
							&val))
					break;
				strncpy(cfg->react.hitl_tools[count], val.ptr,
					HITL_TOOL_NAME_MAX - 1);
			}
			cfg->react.hitl_tools_count = count;
		}
	}

	int legacy_permissions =
		(exec && (nb_toml_table_get_cstr(exec, "network") ||
			  nb_toml_table_get_cstr(exec, "allow_host_execution"))) ||
		(react && (nb_toml_table_get_cstr(react, "hitl_enabled") ||
			   nb_toml_table_get_cstr(react, "hitl_tools") ||
			   nb_toml_table_get_cstr(react, "hitl_auto_approve_readonly") ||
			   nb_toml_table_get_cstr(react, "hitl_auto_approve_media")));
	const nb_toml_value *permissions = table_path(tbl, "permissions");
	if (permissions) {
		char access[32] = "workspace";
		char approval[32] = "on_request";
		char reviewer[32] = "user";

		CFG_STR(permissions, "access", access);
		CFG_STR(permissions, "approval", approval);
		CFG_STR(permissions, "reviewer", reviewer);
		int policy_rc = permission_policy_parse(access, approval, reviewer,
			&cfg->permissions);
		if (policy_rc != 0) {
			nb_toml_cleanup(&parsed);
			return MORPH_ERR_CONFIG;
		}
		cfg->permissions_explicit = 1;
		if (legacy_permissions)
			nb_log_write(logger, NB_LOG_WARN,
				     "[permissions] overrides legacy exec.network, "
				     "exec.allow_host_execution and react.hitl_* settings.");
	} else if (legacy_permissions) {
		cfg->permissions = permission_policy_default();
		nb_log_write(logger, NB_LOG_WARN,
			     "Legacy permissions migrate to "
			     "workspace/on_request/user. Legacy host, network and HITL "
			     "switches no longer grant access; set [permissions] explicitly.");
	}

	const nb_toml_value *context = table_path(tbl, "context");
	if (context) {
		CFG_DBL(context, "summarize_threshold_ratio", cfg->context.summarize_threshold_ratio);
		CFG_DBL(context, "compress_target_ratio", cfg->context.compress_target_ratio);
		CFG_BOOL(context, "in_turn_compaction", cfg->context.in_turn_compaction);
		CFG_INT(context, "protocol_reserve_tokens",
			cfg->context.protocol_reserve_tokens);
		CFG_INT(context, "keep_recent_rounds", cfg->context.keep_recent_rounds);
		CFG_INT(context, "tool_result_max_tokens",
			cfg->context.tool_result_max_tokens);
		CFG_INT(context, "project_doc_max_bytes", cfg->context.project_doc_max_bytes);
		CFG_INT(context, "compaction_user_message_tokens",
			cfg->context.compaction_user_message_tokens);
		CFG_INT(context, "compaction_summary_max_tokens",
			cfg->context.compaction_summary_max_tokens);
		CFG_STR(context, "compaction_prompt_file",
			cfg->context.compaction_prompt_file);
		CFG_INT(context, "compaction_warning_count",
			cfg->context.compaction_warning_count);
	}

	const nb_toml_value *memory = table_path(tbl, "memory");
	if (memory) {
		CFG_BOOL(memory, "enabled", cfg->memory.enabled);
		CFG_BOOL(memory, "hot_path_enabled", cfg->memory.capture_input_enabled);
		CFG_BOOL(memory, "cold_path_enabled", cfg->memory.save_tasks_enabled);
		CFG_BOOL(memory, "llm_extract_enabled", cfg->memory.learn_from_chats_enabled);
		/* Old config keys remain readable; explicit plain names take precedence. */
		CFG_BOOL(memory, "capture_input_enabled", cfg->memory.capture_input_enabled);
		CFG_BOOL(memory, "save_tasks_enabled", cfg->memory.save_tasks_enabled);
		CFG_BOOL(memory, "learn_from_chats_enabled", cfg->memory.learn_from_chats_enabled);
		CFG_INT(memory, "max_facts", cfg->memory.max_facts);
		CFG_INT(memory, "max_episodes", cfg->memory.max_past_tasks);
		CFG_INT(memory, "max_procedures", cfg->memory.max_tips);
		CFG_INT(memory, "max_context_chars", cfg->memory.max_reply_chars);
		CFG_INT(memory, "max_past_tasks", cfg->memory.max_past_tasks);
		CFG_INT(memory, "max_tips", cfg->memory.max_tips);
		CFG_INT(memory, "max_reply_chars", cfg->memory.max_reply_chars);
	}

	const nb_toml_value *render = table_path(tbl, "render");
	if (render) {
		CFG_STR(render, "prefer_image_protocol", cfg->render.prefer_image_protocol);
		CFG_STR(render, "mpv_args", cfg->render.mpv_args);
	}

	const nb_toml_value *ext = table_path(tbl, "ext");
	if (ext) {
		CFG_STR(ext, "dir", cfg->ext.dir);
		CFG_INT(ext, "default_max_memory_mb", cfg->ext.default_max_memory_mb);
		CFG_INT(ext, "default_max_cpu_seconds", cfg->ext.default_max_cpu_seconds);
	}

	const nb_toml_value *dynamic = table_path(tbl, "dynamic_tools");
	if (dynamic) {
		nb_str mode;
		CFG_BOOL(dynamic, "enabled", cfg->dynamic_tools.enabled);
		CFG_STR(dynamic, "runtime", cfg->dynamic_tools.runtime);
		if (nb_toml_get_string(nb_toml_table_get_cstr(dynamic, "mode"), &mode)) {
			strncpy(cfg->dynamic_tools.mode, mode.ptr,
				sizeof(cfg->dynamic_tools.mode) - 1);
			cfg->dynamic_tools.mode_explicit = 1;
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
		load_dynamic_profile(nb_toml_table_get_cstr(dynamic, "local"),
				     &cfg->dynamic_tools.local);
		load_dynamic_profile(nb_toml_table_get_cstr(dynamic, "server"),
				     &cfg->dynamic_tools.server);
	}

	const nb_toml_value *prompt = table_path(tbl, "prompt");
	if (prompt) {
		CFG_STR(prompt, "mode", cfg->prompt.mode);
		CFG_STR(prompt, "system_prompt_file", cfg->prompt.system_prompt_file);
		CFG_STR(prompt, "system_prompt_dir", cfg->prompt.system_prompt_dir);
	}

	const nb_toml_value *skill = table_path(tbl, "skill");
	if (skill) {
		CFG_STR(skill, "dir", cfg->skill.dir);
	}

	const nb_toml_value *sync = table_path(tbl, "sync");
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

	const nb_toml_value *mcp_tbl = table_path(tbl, "mcp");
	const nb_toml_value *mcp_servers =
		mcp_tbl ? nb_toml_table_get_cstr(mcp_tbl, "servers") : NULL;
	if (mcp_servers) {
		int i = 0;
		for (; i < (int)nb_toml_array_size(mcp_servers) && i < MCP_SERVER_MAX; i++) {
			const nb_toml_value *srv = nb_toml_array_get(mcp_servers, (size_t)i);
			if (!srv)
				continue;
			struct config_mcp_server *ms = &cfg->mcp.servers[i];
			CFG_STR(srv, "name", ms->name);
			CFG_STR(srv, "transport", ms->transport);
			CFG_STR(srv, "command", ms->command);
			CFG_STR(srv, "url", ms->http_url);
			CFG_STR(srv, "auth_token_env", ms->http_auth_token_env);
			CFG_STR(srv, "protocol_version", ms->protocol_version);
			CFG_BOOL(srv, "auto_connect", ms->auto_connect);
			CFG_INT(srv, "connect_timeout", ms->connect_timeout);

			const nb_toml_value *args_arr = nb_toml_table_get_cstr(srv, "args");
			if (args_arr) {
				int j = 0;
				for (;
				     j < (int)nb_toml_array_size(args_arr) && j < MCP_CMD_ARGS_MAX;
				     j++) {
					nb_str a;
					if (!nb_toml_get_string(
						    nb_toml_array_get(args_arr, (size_t)(j)), &a))
						break;
					strncpy(ms->args[j], a.ptr, MCP_CMD_ARG_LEN_MAX - 1);
					ms->args_count++;
				}
			}

			const nb_toml_value *env_tbl = nb_toml_table_get_cstr(srv, "env");
			if (env_tbl) {
				int ei = 0;
				for (int ki = 0; ki < 64 && ei < MCP_ENV_MAX; ki++) {
					const char *key =
						nb_toml_table_key_at(env_tbl, (size_t)ki).ptr;
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

	const nb_toml_value *agents = table_path(tbl, "agents");
	if (agents) {
		CFG_BOOL(agents, "enabled", cfg->agents.enabled);
		CFG_INT(agents, "max_threads", cfg->agents.max_threads);
		CFG_INT(agents, "max_depth", cfg->agents.max_depth);
		CFG_INT(agents, "task_timeout_seconds", cfg->agents.task_timeout_seconds);
		CFG_INT(agents, "finalize_wait_seconds", cfg->agents.finalize_wait_seconds);
		CFG_INT(agents, "finalize_grace_seconds", cfg->agents.finalize_grace_seconds);
		if (nb_toml_table_get_cstr(agents, "max_total_tokens") ||
		    nb_toml_table_get_cstr(agents, "budget_input_weight") ||
		    nb_toml_table_get_cstr(agents, "budget_output_weight") ||
		    nb_toml_table_get_cstr(agents, "budget_warning_percent") ||
		    nb_toml_table_get_cstr(agents, "budget_primary_reserve_percent") ||
		    nb_toml_table_get_cstr(agents, "budget_enforcement"))
			nb_log_write(logger, NB_LOG_WARN,
				     "Legacy [agents] budget settings are ignored; "
				     "configure [features.rollout_budget] to enable a limit");
		CFG_STR(agents, "child_shell", cfg->agents.child_shell);
		CFG_STR(agents, "multi_agent_mode", cfg->agents.multi_agent_mode);
	}
	const nb_toml_value *rollout = table_path(tbl, "features.rollout_budget");
	if (rollout) {
		struct config_rollout_budget *budget = &cfg->agents.rollout_budget;
		CFG_BOOL(rollout, "enabled", budget->enabled);
		CFG_INT(rollout, "limit_tokens", budget->limit_tokens);
		CFG_DBL(rollout, "prefill_token_weight", budget->prefill_token_weight);
		CFG_DBL(rollout, "sampling_token_weight", budget->sampling_token_weight);
		const nb_toml_value *reminders =
			nb_toml_table_get_cstr(rollout, "reminder_at_remaining_tokens");
		budget->reminder_count = (int)nb_toml_array_size(reminders);
		for (int i = 0; i < budget->reminder_count; i++)
			budget->reminder_at_remaining_tokens[i] =
				(int)nb_toml_integer(nb_toml_array_get(reminders, (size_t)(i)));
	}
	nb_toml_cleanup(&parsed);

	config_expand_paths(cfg);

	nb_log_write(logger, NB_LOG_INFO, "config loaded from: %s", path);

	return 0;
}

void config_print(const struct config *cfg)
{
	if (!cfg)
		return;
	nb_log_write(cfg ? cfg->logger : NULL, NB_LOG_INFO, "Config:");
	nb_log_write(cfg ? cfg->logger : NULL, NB_LOG_INFO,
		     "  [general] default_session=%s output_dir=%s", cfg->general.default_session,
		     cfg->general.output_dir);
	nb_log_write(cfg ? cfg->logger : NULL, NB_LOG_INFO,
		     "  [model.text] provider=%s model=%s api_base=%s", cfg->models.text.provider,
		     cfg->models.text.model, cfg->models.text.api_base);
	nb_log_write(cfg ? cfg->logger : NULL, NB_LOG_INFO,
		     "  [model.vision] provider=%s model=%s api_base=%s",
		     cfg->models.vision.provider, cfg->models.vision.model,
		     cfg->models.vision.api_base);
	nb_log_write(cfg ? cfg->logger : NULL, NB_LOG_INFO,
		     "  [credits] daily_limit=%d currency=%s cost_coef=%.3f prices=%d",
		     cfg->credits.daily_limit, cfg->credits.currency,
		     cfg->credits.cost_to_credit_coef, cfg->credits.price_count);
	nb_log_write(
		cfg ? cfg->logger : NULL, NB_LOG_INFO,
		"  [react] max_iterations=%d tool_timeout=%d tool_max_retries=%d guardrail=%d/%d "
		"max_empty=%d disabled=%d readonly=%d",
		cfg->react.max_iterations, cfg->react.tool_timeout_seconds,
		cfg->react.tool_max_retries, cfg->react.guardrail_enabled,
		cfg->react.guardrail_max_retries, cfg->react.guardrail_max_empty_rounds,
		cfg->react.disabled_tools_count, cfg->react.readonly_tools_count);
	nb_log_write(cfg->logger, NB_LOG_INFO, "  [permissions] access=%s approval=%s reviewer=%s",
		     permission_access_name(cfg->permissions.access),
		     permission_approval_name(cfg->permissions.approval),
		     permission_reviewer_name(cfg->permissions.reviewer));
	nb_log_write(
		cfg->logger, NB_LOG_INFO,
		"  [agents] threads=%d depth=%d task_timeout=%d finalize_wait=%d finalize_grace=%d",
		cfg->agents.max_threads, cfg->agents.max_depth, cfg->agents.task_timeout_seconds,
		cfg->agents.finalize_wait_seconds, cfg->agents.finalize_grace_seconds);
	nb_log_write(cfg ? cfg->logger : NULL, NB_LOG_INFO,
		     "  [exec] shell=%s timeout_ms=%d yield_ms=%d max_wait_ms=%d "
		     "inline=%d session=%d grace_ms=%d",
		     cfg->exec.shell, cfg->exec.default_timeout_ms, cfg->exec.yield_time_ms,
		     cfg->exec.max_wait_ms, cfg->exec.max_inline_output,
		     cfg->exec.max_session_output, cfg->exec.kill_grace_ms);
	for (int i = 0; i < cfg->react.disabled_tools_count; i++)
		nb_log_write(cfg ? cfg->logger : NULL, NB_LOG_INFO, "    disabled_tool: %s",
			     cfg->react.disabled_tools[i]);
	for (int i = 0; i < cfg->react.readonly_tools_count; i++)
		nb_log_write(cfg ? cfg->logger : NULL, NB_LOG_INFO, "    readonly_tool: %s",
			     cfg->react.readonly_tools[i]);
	nb_log_write(cfg ? cfg->logger : NULL, NB_LOG_INFO,
		     "  [context] threshold=%.1f target=%.1f keep=%d "
		     "in_turn=%d reserve=%d",
		     cfg->context.summarize_threshold_ratio, cfg->context.compress_target_ratio,
		     cfg->context.keep_recent_rounds, cfg->context.in_turn_compaction,
		     cfg->context.protocol_reserve_tokens);
	nb_log_write(cfg ? cfg->logger : NULL, NB_LOG_INFO,
		     "  [memory] enabled=%d capture_input=%d save_tasks=%d learn_from_chats=%d "
		     "facts=%d past_tasks=%d tips=%d "
		     "chars=%d",
		     cfg->memory.enabled, cfg->memory.capture_input_enabled,
		     cfg->memory.save_tasks_enabled, cfg->memory.learn_from_chats_enabled,
		     cfg->memory.max_facts, cfg->memory.max_past_tasks, cfg->memory.max_tips,
		     cfg->memory.max_reply_chars);
}
