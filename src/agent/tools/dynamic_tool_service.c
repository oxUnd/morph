#include "dynamic_tool_service.h"
#include "dynamic_tool_runner.h"
#include "dynamic_tool_store.h"
#include "agent/tool_runtime.h"
#include "util/error.h"
#include "util/file.h"
#include "util/name.h"
#include "util/log.h"
#include "cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int valid_tool_name(const char *name);
static int register_dynamic_tool(struct tool_registry *reg, struct dynamic_tool *dt);
static char *schema_to_string(cJSON *item);
static const char *json_type_name(cJSON *item);
static int append_tool_create_arg_error(morph_buf_t *buf, const char *field, const char *expected,
					cJSON *actual);
static const char *forbidden_source_token(const char *source);
static int validate_tool_create_args(cJSON *root, cJSON *name_item, cJSON *source_item,
				     morph_buf_t *err);
static char *format_check_error(const char *source_path, const char *output);
static int tool_create_fail(struct tool_result *result, const char *stage, int rc,
			    const char *detail);
static int load_tool_from_dir(struct tool_registry *reg, struct tool_context *tctx,
			      const struct config_dynamic_tools *cfg, const char *dir,
			      enum tool_origin origin);
static int create_context_dir(const struct dynamic_tools_context *ctx, const char *name, char *dir,
			      size_t dir_size);
static int dynamic_tool_delete_path_allowed(const struct dynamic_tools_context *ctx,
					    enum tool_origin origin, const char *name,
					    const char *dir);
static int parse_tool_name_arg(const char *args_json, const char *tool_name, cJSON **root_out,
			       char name[TOOL_NAME_MAX]);
static int result_take_cjson(struct tool_result *result, cJSON *root);
static int reload_persistent_if_present(struct dynamic_tools_context *ctx, const char *name);

static int valid_tool_name(const char *name)
{
	if (!name || !*name)
		return 0;
	for (const char *p = name; *p; p++) {
		if (!morph_name_char_valid(*p, "_") || (*p >= 'A' && *p <= 'Z'))
			return 0;
	}
	return 1;
}

static int register_dynamic_tool(struct tool_registry *reg, struct dynamic_tool *dt)
{
	if (reg == NULL || dt == NULL) {
		return -EINVAL;
	}
	struct tool_spec spec = {
		.origin = dt->origin,
		.name = dt->name,
		.description = dt->description,
		.input_schema = dt->input_schema,
		.output_schema = dt->output_schema,
		.exec = dynamic_tool_runner_execute,
		.user_data = dt,
		.user_data_destroy = dynamic_tool_runner_destroy,
		.flags = TOOL_FLAG_DYNAMIC,
		.timeout_seconds = dt->cfg->default_timeout_seconds > 0
					   ? dt->cfg->default_timeout_seconds
					   : tool_context_default_timeout(dt->tctx),
	};
	return tool_register_replace(reg, &spec, TOOL_FLAG_DYNAMIC);
}

static char *schema_to_string(cJSON *item)
{
	if (!item)
		return strdup("{}");
	if (cJSON_IsString(item) && item->valuestring)
		return strdup(item->valuestring);
	return cJSON_PrintUnformatted(item);
}

static const char *json_type_name(cJSON *item)
{
	if (!item)
		return "missing";
	if (cJSON_IsString(item))
		return "string";
	if (cJSON_IsObject(item))
		return "object";
	if (cJSON_IsArray(item))
		return "array";
	if (cJSON_IsNumber(item))
		return "number";
	if (cJSON_IsBool(item))
		return "boolean";
	if (cJSON_IsNull(item))
		return "null";
	return "unknown";
}

static int append_tool_create_arg_error(morph_buf_t *buf, const char *field, const char *expected,
					cJSON *actual)
{
	return morph_buf_printf(buf, "- %s: expected %s, got %s\n", field, expected,
				json_type_name(actual));
}

static int source_identifier_byte(char byte)
{
	return morph_name_char_valid(byte, "_$") || (unsigned char)byte >= 0x80;
}

static const char *forbidden_source_token(const char *source)
{
	static const char *const tokens[] = {
		"require(", "require (", "Buffer",     "process", "child_process",
		"node:",    "__dirname", "__filename", NULL};

	if (!source)
		return NULL;
	for (int i = 0; tokens[i]; i++) {
		const char *found = source;
		size_t length = strlen(tokens[i]);

		while ((found = strstr(found, tokens[i])) != NULL) {
			if ((found == source || !source_identifier_byte(found[-1])) &&
			    (!source_identifier_byte(tokens[i][length - 1]) ||
			     !source_identifier_byte(found[length])))
				return tokens[i];
			found += length;
		}
	}
	return NULL;
}

static int validate_tool_create_args(cJSON *root, cJSON *name_item, cJSON *source_item,
				     morph_buf_t *err)
{
	const char *name;
	const char *token;
	int rc;

	if (!root || !cJSON_IsObject(root)) {
		return morph_buf_puts(err, "tool_create arguments must be a JSON object with "
					   "name, source_js, optional description, and "
					   "input_schema/output_schema.\n");
	}
	rc = 0;
	if (!cJSON_IsString(name_item) || !name_item->valuestring) {
		rc = append_tool_create_arg_error(
			err, "name", "non-empty string matching ^[a-z0-9_]+$", name_item);
	} else if (!valid_tool_name(name_item->valuestring)) {
		rc = morph_buf_printf(err,
				      "- name: invalid value \"%s\"; use only lowercase "
				      "letters, digits, and underscore\n",
				      name_item->valuestring);
	}
	name = cJSON_IsString(name_item) ? name_item->valuestring : "";
	(void)name;
	if (rc != 0)
		return rc;
	if (!cJSON_IsString(source_item) || !source_item->valuestring) {
		return append_tool_create_arg_error(
			err, "source_js", "JavaScript source string defining global run(args)",
			source_item);
	}
	if (!cJSON_GetObjectItem(root, "input_schema")) {
		return append_tool_create_arg_error(err, "input_schema",
						    "JSON Schema object for input arguments", NULL);
	}
	if (!cJSON_GetObjectItem(root, "output_schema")) {
		return append_tool_create_arg_error(err, "output_schema",
						    "JSON Schema object for result data", NULL);
	}
	token = forbidden_source_token(source_item->valuestring);
	if (token) {
		return morph_buf_printf(err,
					"- source_js: forbidden token \"%s\"; dynamic tools "
					"run in QuickJS, not Node.js. Use morph.image, "
					"morph.canvas, morph.fs, morph.fetch, and morph.env "
					"instead.\n",
					token);
	}
	return 0;
}

static char *format_check_error(const char *source_path, const char *output)
{
	morph_buf_t buf;

	if (morph_buf_init(&buf, 1024) != 0)
		return NULL;
	if (morph_buf_printf(
		    &buf,
		    "tool_create failed\n"
		    "stage: check_js\n"
		    "rc: -22\n"
		    "code: EINVAL\n"
		    "detail: JavaScript validation failed for %s.\n\n"
		    "Runner output:\n%s\n\n"
		    "next_action: Fix the source_js and call tool_create again. The script "
		    "must parse, must define global run(args), and run(args) "
		    "must return a JSON-serializable value.",
		    source_path ? source_path : DYN_TOOL_SOURCE_FILE,
		    output && *output ? output : "(runner produced no output)") != 0) {
		morph_buf_cleanup(&buf);
		return NULL;
	}
	return morph_buf_detach(&buf);
}

static int tool_create_fail(struct tool_result *result, const char *stage, int rc,
			    const char *detail)
{
	return tool_result_errorf(result, "tool_failed",
				  "tool_create failed\n"
				  "stage: %s\n"
				  "rc: %d\n"
				  "code: %s\n"
				  "detail: %s\n"
				  "next_action: Fix the field or source for this stage and "
				  "call tool_create again.",
				  stage ? stage : "unknown", rc, morph_strerror(rc),
				  detail && *detail ? detail
						    : "No additional diagnostic was "
						      "provided by this stage.");
}

static int load_tool_from_dir(struct tool_registry *reg, struct tool_context *tctx,
			      const struct config_dynamic_tools *cfg, const char *dir,
			      enum tool_origin origin)
{
	char meta_path[PATH_MAX];
	char source_path[PATH_MAX];
	char *meta_text;
	cJSON *meta;
	struct dynamic_tool *dt;
	char *schema;
	int rc;

	if (!reg || !cfg || !dir)
		return -EINVAL;
	rc = file_path_join(meta_path, sizeof(meta_path), dir, DYN_TOOL_META_FILE);
	if (rc < 0)
		return rc;
	rc = file_path_join(source_path, sizeof(source_path), dir, DYN_TOOL_SOURCE_FILE);
	if (rc < 0)
		return rc;
	if (!file_exists(meta_path))
		return -ENOENT;
	meta_text = file_read_all(meta_path, NULL);
	if (!meta_text)
		return -ENOENT;
	meta = cJSON_Parse(meta_text);
	free(meta_text);
	if (!meta)
		return -EINVAL;
	dt = calloc(1, sizeof(*dt));
	if (!dt) {
		cJSON_Delete(meta);
		return -ENOMEM;
	}
	{
		cJSON *name = cJSON_GetObjectItem(meta, "name");
		cJSON *desc = cJSON_GetObjectItem(meta, "description");
		cJSON *args = cJSON_GetObjectItem(meta, "input_schema");
		cJSON *output = cJSON_GetObjectItem(meta, "output_schema");
		if (!cJSON_IsString(name) || !name->valuestring ||
		    !valid_tool_name(name->valuestring)) {
			cJSON_Delete(meta);
			dynamic_tool_runner_destroy(dt);
			return -EINVAL;
		}
		strncpy(dt->name, name->valuestring, sizeof(dt->name) - 1);
		if (cJSON_IsString(desc) && desc->valuestring)
			strncpy(dt->description, desc->valuestring, sizeof(dt->description) - 1);
		if (!args || (cJSON_IsObject(args) && !cJSON_GetObjectItem(args, "properties"))) {
			schema = strdup(TOOL_EMPTY_INPUT_SCHEMA);
		} else {
			schema = schema_to_string(args);
		}
		if (!schema) {
			cJSON_Delete(meta);
			dynamic_tool_runner_destroy(dt);
			return -ENOMEM;
		}
		strncpy(dt->input_schema, schema, sizeof(dt->input_schema) - 1);
		free(schema);
		if (!output) {
			schema = strdup(TOOL_OBJECT_OUTPUT_SCHEMA);
		} else {
			schema = schema_to_string(output);
		}
		if (!schema) {
			cJSON_Delete(meta);
			dynamic_tool_runner_destroy(dt);
			return -ENOMEM;
		}
		strncpy(dt->output_schema, schema, sizeof(dt->output_schema) - 1);
		free(schema);
	}
	const cJSON *source_field = cJSON_GetObjectItem(meta, "source");
	if (source_field != NULL && !cJSON_IsString(source_field)) {
		cJSON_Delete(meta);
		dynamic_tool_runner_destroy(dt);
		return MORPH_ERR_FORMAT;
	}
	const char *embedded_source = cJSON_GetStringValue(source_field);
	dt->source = embedded_source != NULL ? strdup(embedded_source)
					     : file_read_all(source_path, NULL);
	if (dt->source == NULL) {
		cJSON_Delete(meta);
		dynamic_tool_runner_destroy(dt);
		return -ENOMEM;
	}
	strncpy(dt->source_path, source_path, sizeof(dt->source_path) - 1);
	strncpy(dt->dir, dir, sizeof(dt->dir) - 1);
	dt->cfg = cfg;
	dt->tctx = tctx;
	dt->origin = origin;
	cJSON_Delete(meta);
	if (tool_lookup(reg, dt->name) &&
	    !(tool_lookup(reg, dt->name)->flags & TOOL_FLAG_DYNAMIC)) {
		dynamic_tool_runner_destroy(dt);
		return -EEXIST;
	}
	rc = register_dynamic_tool(reg, dt);
	if (rc < 0) {
		dynamic_tool_runner_destroy(dt);
	}
	return rc;
}

static int create_context_dir(const struct dynamic_tools_context *ctx, const char *name, char *dir,
			      size_t dir_size)
{
	char session_root[PATH_MAX];
	int rc;

	if (!ctx || !ctx->session_id[0])
		return -EINVAL;
	rc = file_path_join(session_root, sizeof(session_root), ctx->cfg->session_dir,
			    ctx->session_id);
	if (rc < 0)
		return rc;
	return file_path_join(dir, dir_size, session_root, name);
}

static int service_create(const char *args_json, struct tool_result *result, void *user_data)
{
	struct dynamic_tools_context *ctx = user_data;
	cJSON *root;
	cJSON *name_item;
	cJSON *desc_item;
	cJSON *source_item;
	char *schema = NULL;
	char *output_schema = NULL;
	char *check_error = NULL;
	struct dynamic_tool *dt = NULL;
	struct tool_entry *existing = NULL;
	struct tool_checkpoint checkpoint;
	char checkpoint_dir[PATH_MAX];
	char new_hash[32];
	int checkpoint_created = 0;
	int register_rc = 0;
	const char *stage = "init";
	int rc;

	if (!ctx || !result)
		return -EINVAL;
	if (!ctx->cfg || !ctx->cfg->enabled)
		return tool_result_error(result, "tool_failed", "dynamic tools disabled");
	stage = "parse_args";
	root = args_json ? cJSON_Parse(args_json) : NULL;
	if (!root)
		return tool_create_fail(result, stage, -EINVAL,
					"tool_create arguments are not valid "
					"JSON.");
	name_item = cJSON_GetObjectItem(root, "name");
	desc_item = cJSON_GetObjectItem(root, "description");
	source_item = cJSON_GetObjectItem(root, "source_js");
	{
		morph_buf_t arg_err;
		if (morph_buf_init(&arg_err, 512) != 0) {
			cJSON_Delete(root);
			return tool_create_fail(result, stage, -ENOMEM,
						"failed to allocate argument "
						"diagnostic buffer");
		}
		rc = validate_tool_create_args(root, name_item, source_item, &arg_err);
		if (rc != 0 || arg_err.len > 0) {
			morph_buf_t detail;
			int out_rc;
			if (morph_buf_init(&detail, 768) != 0) {
				morph_buf_cleanup(&arg_err);
				cJSON_Delete(root);
				return tool_create_fail(result, stage, -ENOMEM,
							"failed to allocate argument detail "
							"buffer");
			}
			(void)morph_buf_printf(&detail,
					       "%s\n"
					       "Expected shape: {\"name\":\"my_tool\","
					       "\"description\":\"...\","
					       "\"input_schema\":{\"type\":\"object\","
					       "\"properties\":{}},"
					       "\"output_schema\":{\"type\":\"object\","
					       "\"properties\":{}},"
					       "\"source_js\":\"async function run(args) { "
					       "return { value: true }; }\"}",
					       morph_buf_cstr(&arg_err));
			out_rc = tool_create_fail(result, stage, -EINVAL, morph_buf_cstr(&detail));
			morph_buf_cleanup(&detail);
			morph_buf_cleanup(&arg_err);
			cJSON_Delete(root);
			return out_rc;
		}
		morph_buf_cleanup(&arg_err);
	}
	stage = "validate_source_size";
	if (ctx->cfg->max_source_bytes < 0 ||
	    strlen(source_item->valuestring) > (size_t)ctx->cfg->max_source_bytes) {
		size_t len = strlen(source_item->valuestring);
		int max_len = ctx->cfg->max_source_bytes;
		morph_buf_t detail;
		int out_rc;
		if (morph_buf_init(&detail, 128) != 0) {
			cJSON_Delete(root);
			return tool_create_fail(result, stage, -ENOMEM,
						"failed to allocate source size "
						"diagnostic buffer");
		}
		(void)morph_buf_printf(&detail,
				       "source_js too large: got %zu bytes, maximum is %d "
				       "bytes. Split the logic or remove embedded data.",
				       len, max_len);
		out_rc = tool_create_fail(result, stage, -EFBIG, morph_buf_cstr(&detail));
		morph_buf_cleanup(&detail);
		cJSON_Delete(root);
		return out_rc;
	}
	stage = "check_name_conflict";
	{
		existing = tool_lookup(ctx->reg, name_item->valuestring);
		if (existing && !(existing->flags & TOOL_FLAG_DYNAMIC)) {
			cJSON_Delete(root);
			return tool_create_fail(result, stage, -EEXIST,
						"tool name already exists and is "
						"not a dynamic tool");
		}
	}
	dynamic_tool_store_hash_text(source_item->valuestring, strlen(source_item->valuestring),
				     new_hash);
	stage = "approval";
	if (ctx->cfg->create_requires_approval && ctx->tctx) {
		struct tool_operation op;
		memset(&op, 0, sizeof(op));
		op.kind = TOOL_OP_EXTERNAL_SEND;
		op.tool_name = "tool_create";
		op.action = "create dynamic tool";
		op.target = name_item->valuestring;
		op.details_json = args_json;
		rc = tool_context_check_operation(ctx->tctx, &op);
		if (rc < 0) {
			cJSON_Delete(root);
			return tool_create_fail(result, stage, rc, "dynamic tool creation denied");
		}
	}
	stage = "validate_input_schema";
	schema = schema_to_string(cJSON_GetObjectItem(root, "input_schema"));
	if (!schema) {
		cJSON_Delete(root);
		return tool_create_fail(result, stage, -ENOMEM, "failed to serialize input_schema");
	}
	{
		cJSON *schema_root = cJSON_Parse(schema);
		if (!schema_root) {
			const char *pos = cJSON_GetErrorPtr();
			morph_buf_t detail;
			int out_rc;
			if (morph_buf_init(&detail, 256) != 0) {
				free(schema);
				cJSON_Delete(root);
				return tool_create_fail(result, stage, -ENOMEM,
							"failed to allocate input_schema "
							"diagnostic buffer");
			}
			(void)morph_buf_printf(&detail,
					       "invalid input_schema JSON near: %.80s\n"
					       "input_schema must be a JSON Schema object or a "
					       "string containing valid JSON Schema.",
					       pos ? pos : "(unknown)");
			out_rc = tool_create_fail(result, stage, -EINVAL, morph_buf_cstr(&detail));
			morph_buf_cleanup(&detail);
			free(schema);
			cJSON_Delete(root);
			return out_rc;
		}
		cJSON_Delete(schema_root);
	}
	stage = "validate_output_schema";
	output_schema = schema_to_string(cJSON_GetObjectItem(root, "output_schema"));
	if (!output_schema) {
		free(schema);
		cJSON_Delete(root);
		return tool_create_fail(result, stage, -ENOMEM,
					"failed to serialize output_schema");
	}
	{
		cJSON *schema_root = cJSON_Parse(output_schema);
		if (!schema_root) {
			free(schema);
			free(output_schema);
			cJSON_Delete(root);
			return tool_create_fail(result, stage, -EINVAL,
						"invalid output_schema JSON");
		}
		cJSON_Delete(schema_root);
	}
	stage = "allocate_tool";
	dt = calloc(1, sizeof(*dt));
	if (!dt) {
		free(schema);
		free(output_schema);
		cJSON_Delete(root);
		return tool_create_fail(result, stage, -ENOMEM, "failed to allocate dynamic tool");
	}
	strncpy(dt->name, name_item->valuestring, sizeof(dt->name) - 1);
	if (cJSON_IsString(desc_item) && desc_item->valuestring)
		strncpy(dt->description, desc_item->valuestring, sizeof(dt->description) - 1);
	else
		snprintf(dt->description, sizeof(dt->description), "Dynamic JS tool %s", dt->name);
	strncpy(dt->input_schema, schema, sizeof(dt->input_schema) - 1);
	free(schema);
	strncpy(dt->output_schema, output_schema, sizeof(dt->output_schema) - 1);
	free(output_schema);
	dt->cfg = ctx->cfg;
	dt->tctx = ctx->tctx;
	dt->origin = TOOL_ORIGIN_DYNAMIC_SESSION;
	stage = "create_tool_dir";
	rc = create_context_dir(ctx, dt->name, dt->dir, sizeof(dt->dir));
	if (rc == 0) {
		stage = "build_source_path";
		rc = file_path_join(dt->source_path, sizeof(dt->source_path), dt->dir,
				    DYN_TOOL_SOURCE_FILE);
	}
	if (rc == 0) {
		stage = "check_js";
		rc = dynamic_tool_runner_check_unpublished_source(source_item->valuestring,
								  &check_error);
	}
	if (rc == 0) {
		stage = "create_checkpoint";
		/* Approval callbacks may have grown or replaced registry entries. */
		existing = tool_lookup(ctx->reg, dt->name);
		if (existing != NULL && !(existing->flags & TOOL_FLAG_DYNAMIC)) {
			rc = -EEXIST;
		} else {
			rc = dynamic_tool_store_create_tool_checkpoint(
				dt->dir, existing ? existing->user_data : NULL, dt->name,
				&checkpoint, checkpoint_dir, sizeof(checkpoint_dir));
		}
		if (rc == 0) {
			strncpy(checkpoint.new_hash, new_hash, sizeof(checkpoint.new_hash) - 1);
			checkpoint_created = 1;
		}
	}
	if (rc == 0) {
		stage = "write_tool_files";
		rc = dynamic_tool_store_write_tool_files(dt, source_item->valuestring);
	}
	if (rc == 0 && check_error) {
		free(check_error);
		check_error = NULL;
	}
	if (rc == 0) {
		stage = "register_tool";
		rc = register_dynamic_tool(ctx->reg, dt);
		register_rc = rc;
	}
	if (rc >= 0 && checkpoint_created) {
		int meta_rc;

		stage = "update_checkpoint";
		meta_rc = dynamic_tool_store_write_checkpoint_meta(checkpoint_dir, &checkpoint);
		if (meta_rc == 0)
			meta_rc = dynamic_tool_store_prune_checkpoint_history(dt->dir);
		if (meta_rc < 0)
			log_warn(result && result->runtime ? result->runtime->http.logger : NULL,
				 "failed to update dynamic tool checkpoint: %s",
				 morph_strerror(meta_rc));
	}
	cJSON_Delete(root);
	if (rc < 0) {
		if (checkpoint_created) {
			(void)dynamic_tool_store_restore_tool_checkpoint(
				dt ? dt->dir : NULL, checkpoint_dir, &checkpoint);
			(void)dynamic_tool_store_remove_tree(checkpoint_dir);
		}
		if (check_error) {
			char *formatted =
				format_check_error(dt ? dt->source_path : NULL, check_error);
			int out_rc;
			dynamic_tool_runner_destroy(dt);
			if (formatted) {
				out_rc = tool_result_error(result, "tool_failed", formatted);
				free(formatted);
			} else {
				out_rc = tool_create_fail(result, stage, rc, check_error);
			}
			free(check_error);
			return out_rc;
		}
		dynamic_tool_runner_destroy(dt);
		return tool_create_fail(result, stage, rc, NULL);
	}
	return tool_result_successf(result,
				    "{\"name\":\"%s\",\"status\":\"%s\","
				    "\"checkpoint_id\":\"%s\","
				    "\"diff_available\":true,"
				    "\"old_hash\":\"%s\","
				    "\"new_hash\":\"%s\"}",
				    dt->name, register_rc > 0 ? "updated" : "registered",
				    checkpoint.id, checkpoint.old_hash, new_hash);
}

static int dynamic_tool_delete_path_allowed(const struct dynamic_tools_context *ctx,
					    enum tool_origin origin, const char *name,
					    const char *dir)
{
	char expected[PATH_MAX];
	char session_root[PATH_MAX];
	char *resolved_dir = NULL;
	char *resolved_expected = NULL;
	int rc;
	int allowed;

	if (!ctx || !ctx->cfg || !name || !dir)
		return 0;
	if (origin == TOOL_ORIGIN_DYNAMIC_SESSION) {
		if (!ctx->session_id[0])
			return 0;
		rc = file_path_join(session_root, sizeof(session_root), ctx->cfg->session_dir,
				    ctx->session_id);
		if (rc < 0)
			return 0;
		rc = file_path_join(expected, sizeof(expected), session_root, name);
	} else if (origin == TOOL_ORIGIN_DYNAMIC_PERSISTENT) {
		rc = file_path_join(expected, sizeof(expected), ctx->cfg->persistent_dir, name);
	} else {
		return 0;
	}
	if (rc < 0)
		return 0;
	resolved_dir = file_resolve_path(dir);
	resolved_expected = file_resolve_path(expected);
	allowed = resolved_dir && resolved_expected && strcmp(resolved_dir, resolved_expected) == 0;
	free(resolved_dir);
	free(resolved_expected);
	return allowed;
}

static int service_promote(const char *args_json, struct tool_result *result, void *user_data)
{
	struct dynamic_tools_context *ctx = user_data;
	cJSON *root;
	cJSON *name_item;
	char src_dir[PATH_MAX];
	char dst_dir[PATH_MAX];
	char src_meta[PATH_MAX];
	char src_js[PATH_MAX];
	char dst_meta[PATH_MAX];
	char dst_js[PATH_MAX];
	char name[TOOL_NAME_MAX];
	int rc;

	if (!ctx || !result)
		return -EINVAL;
	root = args_json ? cJSON_Parse(args_json) : NULL;
	if (!root)
		return tool_result_error(result, "tool_failed", "invalid JSON");
	name_item = cJSON_GetObjectItem(root, "name");
	if (!cJSON_IsString(name_item) || !valid_tool_name(name_item->valuestring)) {
		cJSON_Delete(root);
		return tool_result_error(result, "tool_failed", "tool_promote requires valid name");
	}
	strncpy(name, name_item->valuestring, sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	if (ctx->cfg->promote_requires_approval && ctx->tctx) {
		struct tool_operation op;
		memset(&op, 0, sizeof(op));
		op.kind = TOOL_OP_EXTERNAL_SEND;
		op.tool_name = "tool_promote";
		op.action = "promote dynamic tool";
		op.target = name;
		op.details_json = args_json;
		rc = tool_context_check_operation(ctx->tctx, &op);
		if (rc < 0) {
			cJSON_Delete(root);
			return tool_result_error(result, "tool_failed",
						 "dynamic tool promotion denied");
		}
	}
	rc = create_context_dir(ctx, name, src_dir, sizeof(src_dir));
	if (rc == 0)
		rc = file_path_join(dst_dir, sizeof(dst_dir), ctx->cfg->persistent_dir, name);
	if (rc == 0 && file_exists(dst_dir))
		rc = -EEXIST;
	if (rc == 0)
		rc = file_ensure_dir(dst_dir);
	if (rc == 0)
		rc = file_path_join(src_meta, sizeof(src_meta), src_dir, DYN_TOOL_META_FILE);
	if (rc == 0)
		rc = file_path_join(src_js, sizeof(src_js), src_dir, DYN_TOOL_SOURCE_FILE);
	if (rc == 0)
		rc = file_path_join(dst_meta, sizeof(dst_meta), dst_dir, DYN_TOOL_META_FILE);
	if (rc == 0)
		rc = file_path_join(dst_js, sizeof(dst_js), dst_dir, DYN_TOOL_SOURCE_FILE);
	if (rc == 0) {
		rc = dynamic_tool_store_copy_tool_snapshot(src_meta, src_js, dst_meta, dst_js);
	}
	if (rc == 0)
		rc = load_tool_from_dir(ctx->reg, ctx->tctx, ctx->cfg, dst_dir,
					TOOL_ORIGIN_DYNAMIC_PERSISTENT);
	cJSON_Delete(root);
	if (rc < 0)
		return tool_result_errorf(result, "tool_failed",
					  "failed to promote dynamic tool: %s", morph_strerror(rc));
	return tool_result_successf(result, "{\"name\":\"%s\",\"status\":\"promoted\"}", name);
}

static int service_delete(const char *args_json, struct tool_result *result, void *user_data)
{
	struct dynamic_tools_context *ctx = user_data;
	struct tool_entry *entry;
	struct dynamic_tool *dt;
	enum tool_origin origin;
	cJSON *root;
	cJSON *name_item;
	char name[TOOL_NAME_MAX];
	char dir[PATH_MAX];
	int rc;

	if (!ctx || !result)
		return -EINVAL;
	root = args_json ? cJSON_Parse(args_json) : NULL;
	if (!root)
		return tool_result_error(result, "tool_failed", "invalid JSON");
	name_item = cJSON_GetObjectItem(root, "name");
	if (!cJSON_IsString(name_item) || !valid_tool_name(name_item->valuestring)) {
		cJSON_Delete(root);
		return tool_result_error(result, "tool_failed", "tool_delete requires valid name");
	}
	strncpy(name, name_item->valuestring, sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	entry = tool_lookup(ctx->reg, name);
	if (!entry) {
		cJSON_Delete(root);
		return tool_result_error(result, "tool_failed", "dynamic tool not found");
	}
	if (!(entry->flags & TOOL_FLAG_DYNAMIC) ||
	    (entry->origin != TOOL_ORIGIN_DYNAMIC_SESSION &&
	     entry->origin != TOOL_ORIGIN_DYNAMIC_PERSISTENT)) {
		cJSON_Delete(root);
		return tool_result_error(result, "tool_failed",
					 "tool_delete can only delete dynamic tools");
	}
	dt = entry->user_data;
	if (!dt || !dt->dir[0]) {
		cJSON_Delete(root);
		return tool_result_error(result, "tool_failed", "dynamic tool metadata missing");
	}
	origin = entry->origin;
	strncpy(dir, dt->dir, sizeof(dir) - 1);
	dir[sizeof(dir) - 1] = '\0';
	if (ctx->cfg->promote_requires_approval && ctx->tctx) {
		struct tool_operation op;
		memset(&op, 0, sizeof(op));
		op.kind = TOOL_OP_EXTERNAL_SEND;
		op.tool_name = "tool_delete";
		op.action = "delete dynamic tool";
		op.target = name;
		op.details_json = args_json;
		rc = tool_context_check_operation(ctx->tctx, &op);
		if (rc < 0) {
			cJSON_Delete(root);
			return tool_result_error(result, "tool_failed",
						 "dynamic tool deletion denied");
		}
	}
	if (!dynamic_tool_delete_path_allowed(ctx, origin, name, dir)) {
		cJSON_Delete(root);
		return tool_result_error(result, "tool_failed",
					 "dynamic tool deletion refused: path is outside the "
					 "expected dynamic tools directory");
	}
	rc = dynamic_tool_store_remove_tree(dir);
	if (rc == -ENOENT)
		rc = 0;
	if (rc == 0)
		rc = tool_unregister(ctx->reg, name);
	if (rc == 0 && origin == TOOL_ORIGIN_DYNAMIC_SESSION) {
		char persistent_dir[PATH_MAX];
		int load_rc;

		load_rc = file_path_join(persistent_dir, sizeof(persistent_dir),
					 ctx->cfg->persistent_dir, name);
		if (load_rc == 0 && file_exists(persistent_dir)) {
			load_rc = load_tool_from_dir(ctx->reg, ctx->tctx, ctx->cfg, persistent_dir,
						     TOOL_ORIGIN_DYNAMIC_PERSISTENT);
			if (load_rc < 0)
				rc = load_rc;
		} else if (load_rc < 0) {
			rc = load_rc;
		}
	}
	cJSON_Delete(root);
	if (rc < 0)
		return tool_result_errorf(result, "tool_failed",
					  "failed to delete dynamic tool: %s", morph_strerror(rc));
	return tool_result_successf(result, "{\"name\":\"%s\",\"status\":\"deleted\"}", name);
}

static int parse_tool_name_arg(const char *args_json, const char *tool_name, cJSON **root_out,
			       char name[TOOL_NAME_MAX])
{
	cJSON *root;
	cJSON *name_item;

	if (!root_out || !name)
		return -EINVAL;
	*root_out = NULL;
	root = args_json ? cJSON_Parse(args_json) : NULL;
	if (!root)
		return -EINVAL;
	name_item = cJSON_GetObjectItem(root, "name");
	if (!cJSON_IsString(name_item) || !valid_tool_name(name_item->valuestring)) {
		cJSON_Delete(root);
		return -EINVAL;
	}
	strncpy(name, name_item->valuestring, TOOL_NAME_MAX - 1);
	name[TOOL_NAME_MAX - 1] = '\0';
	*root_out = root;
	(void)tool_name;
	return 0;
}

static int result_take_cjson(struct tool_result *result, cJSON *root)
{
	char *json;

	if (!root)
		return -ENOMEM;
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json)
		return -ENOMEM;
	return tool_result_success_json_text(result, json);
}

static int service_history(const char *args_json, struct tool_result *result, void *user_data)
{
	struct dynamic_tools_context *ctx = user_data;
	cJSON *args = NULL;
	cJSON *limit_item;
	cJSON *out;
	cJSON *items;
	char name[TOOL_NAME_MAX];
	char tool_dir[PATH_MAX];
	char history[PATH_MAX];
	char **dirs = NULL;
	int count = 0;
	int limit = 20;
	int rc;

	if (!ctx || !result)
		return -EINVAL;
	rc = parse_tool_name_arg(args_json, "tool_history", &args, name);
	if (rc < 0)
		return tool_result_error(result, "tool_failed", "tool_history requires valid name");
	limit_item = cJSON_GetObjectItem(args, "limit");
	if (cJSON_IsNumber(limit_item) && limit_item->valueint > 0)
		limit = limit_item->valueint;
	cJSON_Delete(args);
	rc = create_context_dir(ctx, name, tool_dir, sizeof(tool_dir));
	if (rc == 0)
		rc = dynamic_tool_store_history_dir_path(tool_dir, history, sizeof(history));
	if (rc < 0)
		return tool_result_errorf(result, "tool_failed", "failed to read tool history: %s",
					  morph_strerror(rc));
	out = cJSON_CreateObject();
	items = cJSON_CreateArray();
	if (!out || !items) {
		cJSON_Delete(out);
		cJSON_Delete(items);
		return -ENOMEM;
	}
	cJSON_AddStringToObject(out, "name", name);
	cJSON_AddItemToObject(out, "checkpoints", items);
	if (file_exists(history) && file_list_dirs(history, &dirs, &count) == 0) {
		int emitted = 0;

		while (emitted < limit) {
			int max_idx = -1;
			int max_id = -1;

			for (int i = 0; i < count; i++) {
				int value = dynamic_tool_store_checkpoint_id_value(dirs[i]);
				if (value > max_id) {
					max_id = value;
					max_idx = i;
				}
			}
			if (max_idx < 0)
				break;
			{
				char cp_dir[PATH_MAX];
				struct tool_checkpoint cp;
				cJSON *item;

				if (file_path_join(cp_dir, sizeof(cp_dir), history,
						   dirs[max_idx]) == 0 &&
				    dynamic_tool_store_read_checkpoint_meta(cp_dir, &cp) == 0) {
					item = cJSON_CreateObject();
					if (!item) {
						file_free_list(dirs, count);
						cJSON_Delete(out);
						return -ENOMEM;
					}
					cJSON_AddStringToObject(item, "id", cp.id);
					cJSON_AddStringToObject(item, "before_state",
								cp.before_state);
					cJSON_AddStringToObject(item, "origin", cp.origin);
					cJSON_AddStringToObject(item, "old_hash", cp.old_hash);
					cJSON_AddStringToObject(item, "new_hash", cp.new_hash);
					cJSON_AddNumberToObject(item, "created_at",
								(double)cp.created_at);
					cJSON_AddItemToArray(items, item);
					emitted++;
				}
			}
			dirs[max_idx][0] = '\0';
		}
		file_free_list(dirs, count);
	}
	return result_take_cjson(result, out);
}

static int service_diff(const char *args_json, struct tool_result *result, void *user_data)
{
	struct dynamic_tools_context *ctx = user_data;
	struct tool_entry *entry;
	struct dynamic_tool *dt;
	struct tool_checkpoint cp;
	cJSON *args = NULL;
	cJSON *id_item;
	cJSON *out;
	char name[TOOL_NAME_MAX];
	char cp_id[16];
	char cp_dir[PATH_MAX];
	char old_meta_path[PATH_MAX];
	char old_source_path[PATH_MAX];
	char new_meta_path[PATH_MAX];
	char new_source_path[PATH_MAX];
	char *old_meta = NULL;
	char *old_source = NULL;
	char *new_meta = NULL;
	char *new_source = NULL;
	morph_buf_t diff;
	int rc;

	if (!ctx || !result)
		return -EINVAL;
	rc = parse_tool_name_arg(args_json, "tool_diff", &args, name);
	if (rc < 0)
		return tool_result_error(result, "tool_failed", "tool_diff requires valid name");
	id_item = cJSON_GetObjectItem(args, "checkpoint_id");
	if (cJSON_IsString(id_item) && id_item->valuestring &&
	    strcmp(id_item->valuestring, "latest") != 0) {
		strncpy(cp_id, id_item->valuestring, sizeof(cp_id) - 1);
		cp_id[sizeof(cp_id) - 1] = '\0';
	} else {
		char tool_dir[PATH_MAX];
		rc = create_context_dir(ctx, name, tool_dir, sizeof(tool_dir));
		if (rc == 0)
			rc = dynamic_tool_store_latest_checkpoint_id(tool_dir, cp_id,
								     sizeof(cp_id));
		if (rc < 0) {
			cJSON_Delete(args);
			return tool_result_error(result, "tool_failed", "checkpoint not found");
		}
	}
	cJSON_Delete(args);
	entry = tool_lookup(ctx->reg, name);
	if (!entry || !(entry->flags & TOOL_FLAG_DYNAMIC) || !entry->user_data)
		return tool_result_error(result, "tool_failed", "dynamic tool not found");
	dt = entry->user_data;
	rc = dynamic_tool_store_checkpoint_dir_path(dt->dir, cp_id, cp_dir, sizeof(cp_dir));
	if (rc == 0)
		rc = dynamic_tool_store_read_checkpoint_meta(cp_dir, &cp);
	if (rc == 0)
		rc = dynamic_tool_store_tool_file_paths(cp_dir, old_meta_path,
							sizeof(old_meta_path), old_source_path,
							sizeof(old_source_path));
	if (rc == 0)
		rc = dynamic_tool_store_tool_file_paths(dt->dir, new_meta_path,
							sizeof(new_meta_path), new_source_path,
							sizeof(new_source_path));
	if (rc < 0)
		return tool_result_error(result, "tool_failed", "checkpoint not found");
	old_meta = dynamic_tool_store_read_optional_file(old_meta_path);
	old_source = dynamic_tool_store_read_optional_file(old_source_path);
	new_meta = dynamic_tool_store_read_optional_file(new_meta_path);
	new_source = dynamic_tool_store_read_optional_file(new_source_path);
	if (!old_meta || !old_source || !new_meta || !new_source) {
		free(old_meta);
		free(old_source);
		free(new_meta);
		free(new_source);
		return -ENOMEM;
	}
	rc = morph_buf_init(&diff, 4096);
	if (rc == 0)
		rc = dynamic_tool_store_append_whole_file_diff(&diff, DYN_TOOL_META_FILE, old_meta,
							       new_meta);
	if (rc == 0)
		rc = dynamic_tool_store_append_whole_file_diff(&diff, DYN_TOOL_SOURCE_FILE,
							       old_source, new_source);
	free(old_meta);
	free(old_source);
	free(new_meta);
	free(new_source);
	if (rc != 0) {
		morph_buf_cleanup(&diff);
		return rc;
	}
	out = cJSON_CreateObject();
	if (!out) {
		morph_buf_cleanup(&diff);
		return -ENOMEM;
	}
	cJSON_AddStringToObject(out, "name", name);
	cJSON_AddStringToObject(out, "checkpoint_id", cp.id);
	cJSON_AddStringToObject(out, "before_state", cp.before_state);
	cJSON_AddStringToObject(out, "diff", morph_buf_cstr(&diff));
	morph_buf_cleanup(&diff);
	return result_take_cjson(result, out);
}

static int reload_persistent_if_present(struct dynamic_tools_context *ctx, const char *name)
{
	char persistent_dir[PATH_MAX];
	int rc;

	rc = file_path_join(persistent_dir, sizeof(persistent_dir), ctx->cfg->persistent_dir, name);
	if (rc < 0)
		return rc;
	if (!file_exists(persistent_dir))
		return 0;
	rc = load_tool_from_dir(ctx->reg, ctx->tctx, ctx->cfg, persistent_dir,
				TOOL_ORIGIN_DYNAMIC_PERSISTENT);
	return rc < 0 ? rc : 0;
}

static int service_rollback(const char *args_json, struct tool_result *result, void *user_data)
{
	struct dynamic_tools_context *ctx = user_data;
	struct tool_entry *entry;
	struct dynamic_tool *dt;
	struct tool_checkpoint cp;
	cJSON *args = NULL;
	cJSON *id_item;
	char name[TOOL_NAME_MAX];
	char cp_id[16];
	char tool_dir[PATH_MAX];
	char cp_dir[PATH_MAX];
	char source_path[PATH_MAX];
	char *check_error = NULL;
	int rc;

	if (!ctx || !result)
		return -EINVAL;
	rc = parse_tool_name_arg(args_json, "tool_rollback", &args, name);
	if (rc < 0)
		return tool_result_error(result, "tool_failed",
					 "tool_rollback requires valid name");
	id_item = cJSON_GetObjectItem(args, "checkpoint_id");
	if (cJSON_IsString(id_item) && id_item->valuestring &&
	    strcmp(id_item->valuestring, "latest") != 0) {
		strncpy(cp_id, id_item->valuestring, sizeof(cp_id) - 1);
		cp_id[sizeof(cp_id) - 1] = '\0';
	} else {
		rc = create_context_dir(ctx, name, tool_dir, sizeof(tool_dir));
		if (rc == 0)
			rc = dynamic_tool_store_latest_checkpoint_id(tool_dir, cp_id,
								     sizeof(cp_id));
		if (rc < 0) {
			cJSON_Delete(args);
			return tool_result_error(result, "tool_failed", "checkpoint not found");
		}
	}
	cJSON_Delete(args);
	entry = tool_lookup(ctx->reg, name);
	if (!entry || !(entry->flags & TOOL_FLAG_DYNAMIC) || !entry->user_data)
		return tool_result_error(result, "tool_failed", "dynamic tool not found");
	dt = entry->user_data;
	strncpy(tool_dir, dt->dir, sizeof(tool_dir) - 1);
	tool_dir[sizeof(tool_dir) - 1] = '\0';
	rc = dynamic_tool_store_checkpoint_dir_path(tool_dir, cp_id, cp_dir, sizeof(cp_dir));
	if (rc == 0)
		rc = dynamic_tool_store_read_checkpoint_meta(cp_dir, &cp);
	if (rc < 0)
		return tool_result_error(result, "tool_failed", "checkpoint not found");
	rc = dynamic_tool_store_restore_tool_checkpoint(tool_dir, cp_dir, &cp);
	if (rc < 0)
		return tool_result_errorf(result, "tool_failed", "failed to restore checkpoint: %s",
					  morph_strerror(rc));
	if (strcmp(cp.before_state, "absent") == 0) {
		rc = tool_unregister(ctx->reg, name);
		if (rc == -ENOENT)
			rc = 0;
		if (rc == 0)
			rc = reload_persistent_if_present(ctx, name);
		if (rc < 0)
			return tool_result_errorf(result, "tool_failed",
						  "failed to unregister rolled back tool: %s",
						  morph_strerror(rc));
		return tool_result_successf(result,
					    "{\"name\":\"%s\",\"status\":\"rolled_back\","
					    "\"checkpoint_id\":\"%s\",\"state\":\"absent\"}",
					    name, cp.id);
	}
	rc = file_path_join(source_path, sizeof(source_path), tool_dir, DYN_TOOL_SOURCE_FILE);
	if (rc == 0)
		rc = dynamic_tool_runner_check_js_source(source_path, &check_error);
	if (rc < 0) {
		int out_rc;
		out_rc = tool_result_errorf(result, "tool_failed",
					    "restored checkpoint failed validation: %s",
					    check_error ? check_error : morph_strerror(rc));
		free(check_error);
		return out_rc;
	}
	free(check_error);
	rc = load_tool_from_dir(ctx->reg, ctx->tctx, ctx->cfg, tool_dir,
				TOOL_ORIGIN_DYNAMIC_SESSION);
	if (rc < 0)
		return tool_result_errorf(result, "tool_failed",
					  "failed to register rolled back tool: %s",
					  morph_strerror(rc));
	(void)dynamic_tool_store_remove_checkpoint_ids_from(
		tool_dir, dynamic_tool_store_checkpoint_id_value(cp.id));
	return tool_result_successf(result,
				    "{\"name\":\"%s\",\"status\":\"rolled_back\","
				    "\"checkpoint_id\":\"%s\",\"state\":\"present\"}",
				    name, cp.id);
}

int dynamic_tool_service_load_root(struct tool_registry *reg, struct tool_context *tctx,
				   const struct config_dynamic_tools *cfg, const char *root,
				   enum tool_origin origin)
{
	char **dirs = NULL;
	int count = 0;

	if (!reg || !cfg || !cfg->enabled || !root)
		return 0;
	if (!file_exists(root))
		return 0;
	if (file_list_dirs(root, &dirs, &count) != 0)
		return 0;
	for (int i = 0; i < count; i++) {
		char path[PATH_MAX];
		if (dirs[i][0] == '.')
			continue;
		if (file_path_join(path, sizeof(path), root, dirs[i]) != 0)
			continue;
		(void)load_tool_from_dir(reg, tctx, cfg, path, origin);
	}
	file_free_list(dirs, count);
	return 0;
}

int dynamic_tool_service_load_session(struct tool_registry *reg, struct tool_context *tctx,
				      const struct config_dynamic_tools *cfg,
				      const char *session_id)
{
	char session_root[PATH_MAX];
	int rc;

	if (!cfg || !cfg->enabled || !session_id || !*session_id)
		return 0;
	rc = file_path_join(session_root, sizeof(session_root), cfg->session_dir, session_id);
	if (rc < 0)
		return rc;
	return dynamic_tool_service_load_root(reg, tctx, cfg, session_root,
					      TOOL_ORIGIN_DYNAMIC_SESSION);
}

int dynamic_tool_service_execute(struct dynamic_tools_context *context,
				 enum dynamic_tool_action action, const char *args,
				 struct tool_result *result)
{
	int rc;

	if (!context || !result)
		return -EINVAL;
	pthread_mutex_lock(&context->reg->mutation_mutex);
	if (dynamic_tool_runner_cancelled(result)) {
		pthread_mutex_unlock(&context->reg->mutation_mutex);
		return -ECANCELED;
	}
	switch (action) {
	case DYNAMIC_TOOL_CREATE:
		rc = service_create(args, result, context);
		break;
	case DYNAMIC_TOOL_PROMOTE:
		rc = service_promote(args, result, context);
		break;
	case DYNAMIC_TOOL_DELETE:
		rc = service_delete(args, result, context);
		break;
	case DYNAMIC_TOOL_HISTORY:
		rc = service_history(args, result, context);
		break;
	case DYNAMIC_TOOL_DIFF:
		rc = service_diff(args, result, context);
		break;
	case DYNAMIC_TOOL_ROLLBACK:
		rc = service_rollback(args, result, context);
		break;
	default:
		rc = -EINVAL;
		break;
	}
	pthread_mutex_unlock(&context->reg->mutation_mutex);
	return rc;
}
