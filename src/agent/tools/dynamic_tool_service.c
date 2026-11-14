#include "dynamic_tool_service.h"
#include "nb/utf8.h"
#include <limits.h>
#include "cJSON.h"
#include "nb/cleanup.h"
#include "dynamic_tool_runner.h"
#include "dynamic_tool_store.h"
#include "agent/tool_runtime.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/str.h"
#include "nb/log.h"
#include "cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int valid_tool_name(const char *name);
static int register_dynamic_tool_take(struct tool_registry *reg, struct dynamic_tool **owner);
static char *schema_to_string(cJSON *item);
static const char *json_type_name(cJSON *item);
static int append_tool_create_arg_error(nb_buf *buf, const char *field, const char *expected,
					cJSON *actual);
static const char *forbidden_source_token(const char *source);
static int validate_tool_create_args(cJSON *root, cJSON *name_item, cJSON *source_item,
				     nb_buf *err);
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
static int result_take_cjson(struct tool_result *result, cJSON **root);
static int reload_persistent_if_present(struct dynamic_tools_context *ctx, const char *name);

static int valid_tool_name(const char *name)
{
	if (!name || !*name || strlen(name) >= TOOL_NAME_MAX)
		return 0;
	for (const char *p = name; *p; p++) {
		if (!nb_ascii_is_alnum_or((unsigned char)(*p), "_") || (*p >= 'A' && *p <= 'Z'))
			return 0;
	}
	return 1;
}

static int register_dynamic_tool_take(struct tool_registry *reg, struct dynamic_tool **owner)
{
	if (!reg || !owner || !*owner)
		return NB_ERR_INVAL;
	struct dynamic_tool *dt = *owner;
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
	int rc = tool_register_replace_take(reg, &spec, TOOL_FLAG_DYNAMIC);
	if (rc >= 0)
		*owner = NULL;
	return rc;
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

static int append_tool_create_arg_error(nb_buf *buf, const char *field, const char *expected,
					cJSON *actual)
{
	return (nb_buf_printf(buf, "- %s: expected %s, got %s\n", field, expected,
			      json_type_name(actual))
			? 0
			: NB_ERR_NOMEM);
}

static int source_identifier_byte(char byte)
{
	return nb_str_is_alnum(nb_str_from_parts(&(byte), 1), "_$") || (unsigned char)byte >= 0x80;
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

static int validate_tool_create_args(cJSON *root, cJSON *name_item, cJSON *source_item, nb_buf *err)
{
	const char *name;
	const char *token;
	int rc;

	if (!root || !cJSON_IsObject(root)) {
		return (nb_buf_append_cstr(err, "tool_create arguments must be a JSON object with "
						"name, source_js, optional description, and "
						"input_schema/output_schema.\n")
				? 0
				: NB_ERR_NOMEM);
	}
	rc = 0;
	if (!cJSON_IsString(name_item) || !name_item->valuestring) {
		rc = append_tool_create_arg_error(
			err, "name", "non-empty string matching ^[a-z0-9_]+$", name_item);
	} else if (!valid_tool_name(name_item->valuestring)) {
		rc = (nb_buf_printf(err,
				    "- name: invalid value \"%s\"; use only lowercase "
				    "letters, digits, and underscore, with at most %d bytes\n",
				    name_item->valuestring, TOOL_NAME_MAX - 1)
			      ? 0
			      : NB_ERR_NOMEM);
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
		return (nb_buf_printf(err,
				      "- source_js: forbidden token \"%s\"; dynamic tools "
				      "run in QuickJS, not Node.js. Use morph.image, "
				      "morph.canvas, morph.fs, morph.fetch, and morph.env "
				      "instead.\n",
				      token)
				? 0
				: NB_ERR_NOMEM);
	}
	return 0;
}

static char *format_check_error(const char *source_path, const char *output)
{
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};

	if ((nb_buf_init(&buf), nb_buf_reserve(&buf, 1024) ? 0 : NB_ERR_NOMEM) != 0)
		return NULL;
	if ((nb_buf_printf(&buf,
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
			   output && *output ? output : "(runner produced no output)")
		     ? 0
		     : NB_ERR_NOMEM) != 0) {
		nb_buf_cleanup(&buf);
		return NULL;
	}
	return (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
}

static int tool_create_fail(struct tool_result *result, const char *stage, int rc,
			    const char *detail)
{
	return tool_result_error_format(
		result, "tool_failed",
		"tool_create failed\n"
		"stage: %s\n"
		"rc: %d\n"
		"code: %s\n"
		"detail: %s\n"
		"next_action: Fix the field or source for this stage and "
		"call tool_create again.",
		stage ? stage : "unknown", rc,
		nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc),
		detail && *detail ? detail
				  : "No additional diagnostic was "
				    "provided by this stage.");
}

static int load_tool_from_dir(struct tool_registry *reg, struct tool_context *tctx,
			      const struct config_dynamic_tools *cfg, const char *dir,
			      enum tool_origin origin)
{
	struct dynamic_tool_definition definition;
	struct dynamic_tool *dt;
	char meta_path[PATH_MAX];
	char source_path[PATH_MAX];
	int rc;

	if (!reg || !cfg || !dir)
		return NB_ERR_INVAL;
	rc = dynamic_tool_store_tool_file_paths(dir, meta_path, sizeof(meta_path), source_path,
						sizeof(source_path));
	if (rc != 0)
		return rc;
	rc = dynamic_tool_store_load_definition(meta_path, source_path, &definition);
	if (rc != 0)
		return rc;
	if (!valid_tool_name(definition.name)) {
		dynamic_tool_store_definition_cleanup(&definition);
		return NB_ERR_INVAL;
	}
	dt = calloc(1, sizeof(*dt));
	if (!dt) {
		dynamic_tool_store_definition_cleanup(&definition);
		return NB_ERR_NOMEM;
	}
	memcpy(dt->name, definition.name, strlen(definition.name) + 1);
	dt->description = strdup(definition.description ? definition.description : "");
	dt->input_schema = strdup(definition.input_schema);
	dt->output_schema = strdup(definition.output_schema);
	dt->source = strdup(definition.source);
	dynamic_tool_store_definition_cleanup(&definition);
	if (!dt->source || !dt->description || !dt->input_schema || !dt->output_schema) {
		dynamic_tool_runner_destroy(dt);
		return NB_ERR_NOMEM;
	}
	memcpy(dt->source_path, source_path, strlen(source_path) + 1);
	if (strlen(dir) >= sizeof(dt->dir)) {
		dynamic_tool_runner_destroy(dt);
		return NB_ERR_NAME_TOO_LONG;
	}
	memcpy(dt->dir, dir, strlen(dir) + 1);
	dt->cfg = cfg;
	dt->tctx = tctx;
	dt->origin = origin;
	if (tool_borrow(reg, dt->name) &&
	    !(tool_borrow(reg, dt->name)->flags & TOOL_FLAG_DYNAMIC)) {
		dynamic_tool_runner_destroy(dt);
		return NB_ERR_EXISTS;
	}
	rc = register_dynamic_tool_take(reg, &dt);
	if (rc < 0)
		dynamic_tool_runner_destroy(dt);
	return rc;
}

static int create_context_dir(const struct dynamic_tools_context *ctx, const char *name, char *dir,
			      size_t dir_size)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_2 = NB_OK;

	char session_root[PATH_MAX];
	int rc;

	if (!ctx || !ctx->session_id[0])
		return NB_ERR_INVAL;
	rc = ((file_status_1 = nb_path_concat(&file_value_1, ctx->cfg->session_dir,
					      ctx->session_id)) != NB_OK
		      ? file_status_1
	      : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), session_root,
						sizeof(session_root))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_1);
	if (rc < 0)
		return rc;
	return ((file_status_2 = nb_path_concat(&file_value_2, session_root, name)) != NB_OK
			? file_status_2
		: (file_status_2 = nb_str_copy_to(nb_buf_str(&file_value_2), dir, dir_size)) ==
				NB_ERR_RANGE
			? NB_ERR_NAME_TOO_LONG
			: file_status_2);
}

static int service_create(const char *args_json, struct tool_result *result, void *user_data)
{
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_3 = NB_OK;

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
		return NB_ERR_INVAL;
	if (!ctx->cfg || !ctx->cfg->enabled)
		return tool_result_error_copy(result, "tool_failed", "dynamic tools disabled");
	stage = "parse_args";
	root = args_json ? cJSON_Parse(args_json) : NULL;
	if (!root)
		return tool_create_fail(result, stage, NB_ERR_INVAL,
					"tool_create arguments are not valid "
					"JSON.");
	name_item = cJSON_GetObjectItem(root, "name");
	desc_item = cJSON_GetObjectItem(root, "description");
	source_item = cJSON_GetObjectItem(root, "source_js");
	{
		nb_buf arg_err NB_CLEANUP(nb_buf_cleanup) = {0};
		if ((nb_buf_init(&arg_err), nb_buf_reserve(&arg_err, 512) ? 0 : NB_ERR_NOMEM) !=
		    0) {
			cJSON_Delete(root);
			return tool_create_fail(result, stage, NB_ERR_NOMEM,
						"failed to allocate argument "
						"diagnostic buffer");
		}
		rc = validate_tool_create_args(root, name_item, source_item, &arg_err);
		if (rc != 0 || arg_err.len > 0) {
			nb_buf detail NB_CLEANUP(nb_buf_cleanup) = {0};
			int out_rc;
			if ((nb_buf_init(&detail),
			     nb_buf_reserve(&detail, 768) ? 0 : NB_ERR_NOMEM) != 0) {
				nb_buf_cleanup(&arg_err);
				cJSON_Delete(root);
				return tool_create_fail(result, stage, NB_ERR_NOMEM,
							"failed to allocate argument detail "
							"buffer");
			}
			(void)nb_buf_printf(&detail,
					    "%s\n"
					    "Expected shape: {\"name\":\"my_tool\","
					    "\"description\":\"...\","
					    "\"input_schema\":{\"type\":\"object\","
					    "\"properties\":{}},"
					    "\"output_schema\":{\"type\":\"object\","
					    "\"properties\":{}},"
					    "\"source_js\":\"async function run(args) { "
					    "return { value: true }; }\"}",
					    nb_buf_cstr(&arg_err));
			out_rc =
				tool_create_fail(result, stage, NB_ERR_INVAL, nb_buf_cstr(&detail));
			nb_buf_cleanup(&detail);
			nb_buf_cleanup(&arg_err);
			cJSON_Delete(root);
			return out_rc;
		}
		nb_buf_cleanup(&arg_err);
	}
	stage = "validate_source_size";
	if (ctx->cfg->max_source_bytes < 0 ||
	    strlen(source_item->valuestring) > (size_t)ctx->cfg->max_source_bytes) {
		size_t len = strlen(source_item->valuestring);
		int max_len = ctx->cfg->max_source_bytes;
		nb_buf detail NB_CLEANUP(nb_buf_cleanup) = {0};
		int out_rc;
		if ((nb_buf_init(&detail), nb_buf_reserve(&detail, 128) ? 0 : NB_ERR_NOMEM) != 0) {
			cJSON_Delete(root);
			return tool_create_fail(result, stage, NB_ERR_NOMEM,
						"failed to allocate source size "
						"diagnostic buffer");
		}
		(void)nb_buf_printf(&detail,
				    "source_js too large: got %zu bytes, maximum is %d "
				    "bytes. Split the logic or remove embedded data.",
				    len, max_len);
		out_rc = tool_create_fail(result, stage, NB_ERR_TOO_BIG, nb_buf_cstr(&detail));
		nb_buf_cleanup(&detail);
		cJSON_Delete(root);
		return out_rc;
	}
	stage = "check_name_conflict";
	{
		existing = tool_borrow(ctx->reg, name_item->valuestring);
		if (existing && !(existing->flags & TOOL_FLAG_DYNAMIC)) {
			cJSON_Delete(root);
			return tool_create_fail(result, stage, NB_ERR_EXISTS,
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
		return tool_create_fail(result, stage, NB_ERR_NOMEM,
					"failed to serialize input_schema");
	}
	{
		cJSON *schema_root = cJSON_Parse(schema);
		if (!schema_root) {
			const char *pos = cJSON_GetErrorPtr();
			nb_buf detail NB_CLEANUP(nb_buf_cleanup) = {0};
			int out_rc;
			if ((nb_buf_init(&detail),
			     nb_buf_reserve(&detail, 256) ? 0 : NB_ERR_NOMEM) != 0) {
				free(schema);
				cJSON_Delete(root);
				return tool_create_fail(result, stage, NB_ERR_NOMEM,
							"failed to allocate input_schema "
							"diagnostic buffer");
			}
			(void)nb_buf_printf(&detail,
					    "invalid input_schema JSON near: %.80s\n"
					    "input_schema must be a JSON Schema object or a "
					    "string containing valid JSON Schema.",
					    pos ? pos : "(unknown)");
			out_rc =
				tool_create_fail(result, stage, NB_ERR_INVAL, nb_buf_cstr(&detail));
			nb_buf_cleanup(&detail);
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
		return tool_create_fail(result, stage, NB_ERR_NOMEM,
					"failed to serialize output_schema");
	}
	{
		cJSON *schema_root = cJSON_Parse(output_schema);
		if (!schema_root) {
			free(schema);
			free(output_schema);
			cJSON_Delete(root);
			return tool_create_fail(result, stage, NB_ERR_INVAL,
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
		return tool_create_fail(result, stage, NB_ERR_NOMEM,
					"failed to allocate dynamic tool");
	}
	memcpy(dt->name, name_item->valuestring, strlen(name_item->valuestring) + 1);
	if (cJSON_IsString(desc_item) && desc_item->valuestring)
		dt->description = strdup(desc_item->valuestring);
	else {
		nb_buf description NB_CLEANUP(nb_buf_cleanup) = {0};
		if ((nb_buf_printf(&description, "Dynamic JS tool %s", dt->name)
			     ? 0
			     : NB_ERR_NOMEM) == 0)
			dt->description =
				(nb_buf_ok(&description) ? strdup(nb_buf_cstr(&description))
							 : NULL);
	}
	dt->input_schema = schema;
	dt->output_schema = output_schema;
	if (!dt->description) {
		dynamic_tool_runner_destroy(dt);
		cJSON_Delete(root);
		return tool_create_fail(result, stage, NB_ERR_NOMEM,
					"failed to copy tool description");
	}
	dt->cfg = ctx->cfg;
	dt->tctx = ctx->tctx;
	dt->origin = TOOL_ORIGIN_DYNAMIC_SESSION;
	stage = "create_tool_dir";
	rc = create_context_dir(ctx, dt->name, dt->dir, sizeof(dt->dir));
	if (rc == 0) {
		stage = "build_source_path";
		rc = ((file_status_3 = nb_path_concat(&file_value_3, dt->dir,
						      DYN_TOOL_SOURCE_FILE)) != NB_OK
			      ? file_status_3
		      : (file_status_3 = nb_str_copy_to(nb_buf_str(&file_value_3), dt->source_path,
							sizeof(dt->source_path))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_3);
	}
	if (rc == 0) {
		stage = "check_js";
		rc = dynamic_tool_runner_check_unpublished_source(source_item->valuestring,
								  &check_error);
	}
	if (rc == 0) {
		stage = "create_checkpoint";
		/* Approval callbacks may have grown or replaced registry entries. */
		existing = tool_borrow(ctx->reg, dt->name);
		if (existing != NULL && !(existing->flags & TOOL_FLAG_DYNAMIC)) {
			rc = NB_ERR_EXISTS;
		} else {
			rc = dynamic_tool_store_create_tool_checkpoint(
				dt->dir, existing ? existing->user_data : NULL, dt->name,
				&checkpoint, checkpoint_dir, sizeof(checkpoint_dir));
		}
		if (rc == 0) {
			(void)nb_str_copy_to(
				nb_str_from_parts(
					new_hash,
					nb_utf8_clamp_bytes(new_hash,
							    sizeof(checkpoint.new_hash) - 1)),
				checkpoint.new_hash, (sizeof(checkpoint.new_hash) - 1) + 1);
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
	char published_name[TOOL_NAME_MAX] = {0};
	char published_dir[PATH_MAX] = {0};
	if (rc == 0) {
		stage = "register_tool";
		snprintf(published_name, sizeof(published_name), "%s", dt->name);
		snprintf(published_dir, sizeof(published_dir), "%s", dt->dir);
		rc = register_dynamic_tool_take(ctx->reg, &dt);
		register_rc = rc;
	}
	if (rc >= 0 && checkpoint_created) {
		int meta_rc;

		stage = "update_checkpoint";
		meta_rc = dynamic_tool_store_write_checkpoint_meta(checkpoint_dir, &checkpoint);
		if (meta_rc == 0)
			meta_rc = dynamic_tool_store_prune_checkpoint_history(published_dir);
		if (meta_rc < 0)
			nb_log_write(
				result && result->runtime ? result->runtime->http.logger : NULL,
				NB_LOG_WARN, "failed to update dynamic tool checkpoint: %s",
				nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, meta_rc));
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
				out_rc = tool_result_error_copy(result, "tool_failed", formatted);
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
	return tool_result_success_format(result,
					  "{\"name\":\"%s\",\"status\":\"%s\","
					  "\"checkpoint_id\":\"%s\","
					  "\"diff_available\":true,"
					  "\"old_hash\":\"%s\","
					  "\"new_hash\":\"%s\"}",
					  published_name,
					  register_rc > 0 ? "updated" : "registered", checkpoint.id,
					  checkpoint.old_hash, new_hash);
}

static int dynamic_tool_delete_path_allowed(const struct dynamic_tools_context *ctx,
					    enum tool_origin origin, const char *name,
					    const char *dir)
{
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_4 = NB_OK;
	nb_buf file_value_5 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_5 = NB_OK;
	nb_buf file_value_6 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_6 = NB_OK;
	nb_buf file_value_7 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_8 NB_CLEANUP(nb_buf_destroy) = {0};

	char expected[PATH_MAX];
	char session_root[PATH_MAX];
	int rc;

	if (!ctx || !ctx->cfg || !name || !dir)
		return 0;
	if (origin == TOOL_ORIGIN_DYNAMIC_SESSION) {
		if (!ctx->session_id[0])
			return 0;
		rc = ((file_status_4 = nb_path_concat(&file_value_4, ctx->cfg->session_dir,
						      ctx->session_id)) != NB_OK
			      ? file_status_4
		      : (file_status_4 = nb_str_copy_to(nb_buf_str(&file_value_4), session_root,
							sizeof(session_root))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_4);
		if (rc < 0)
			return 0;
		rc = ((file_status_5 = nb_path_concat(&file_value_5, session_root, name)) != NB_OK
			      ? file_status_5
		      : (file_status_5 = nb_str_copy_to(nb_buf_str(&file_value_5), expected,
							sizeof(expected))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_5);
	} else if (origin == TOOL_ORIGIN_DYNAMIC_PERSISTENT) {
		rc = ((file_status_6 = nb_path_concat(&file_value_6, ctx->cfg->persistent_dir,
						      name)) != NB_OK
			      ? file_status_6
		      : (file_status_6 = nb_str_copy_to(nb_buf_str(&file_value_6), expected,
							sizeof(expected))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_6);
	} else {
		return 0;
	}
	if (rc < 0)
		return 0;
	if (nb_path_expand(&file_value_7, dir, NULL) != NB_OK ||
	    nb_path_canonical_missing(&file_value_7, nb_buf_cstr(&file_value_7), NULL) != NB_OK ||
	    nb_path_expand(&file_value_8, expected, NULL) != NB_OK ||
	    nb_path_canonical_missing(&file_value_8, nb_buf_cstr(&file_value_8), NULL) != NB_OK)
		return 0;
	return strcmp(nb_buf_cstr(&file_value_7), nb_buf_cstr(&file_value_8)) == 0;
}

static int service_promote(const char *args_json, struct tool_result *result, void *user_data)
{
	nb_buf file_value_9 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_9 = NB_OK;
	nb_buf file_value_10 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_10 = NB_OK;
	nb_buf file_value_11 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_11 = NB_OK;
	nb_buf file_value_12 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_12 = NB_OK;
	nb_buf file_value_13 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_13 = NB_OK;

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
		return NB_ERR_INVAL;
	root = args_json ? cJSON_Parse(args_json) : NULL;
	if (!root)
		return tool_result_error_copy(result, "tool_failed", "invalid JSON");
	name_item = cJSON_GetObjectItem(root, "name");
	if (!cJSON_IsString(name_item) || !valid_tool_name(name_item->valuestring)) {
		cJSON_Delete(root);
		return tool_result_error_copy(result, "tool_failed",
					      "tool_promote requires valid name");
	}
	(void)nb_str_copy_to(
		nb_str_from_parts(name_item->valuestring,
				  nb_utf8_clamp_bytes(name_item->valuestring, sizeof(name) - 1)),
		name, (sizeof(name) - 1) + 1);
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
			return tool_result_error_copy(result, "tool_failed",
						      "dynamic tool promotion denied");
		}
	}
	rc = create_context_dir(ctx, name, src_dir, sizeof(src_dir));
	if (rc == 0)
		rc = ((file_status_9 = nb_path_concat(&file_value_9, ctx->cfg->persistent_dir,
						      name)) != NB_OK
			      ? file_status_9
		      : (file_status_9 = nb_str_copy_to(nb_buf_str(&file_value_9), dst_dir,
							sizeof(dst_dir))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_9);
	if (rc == 0 && nb_file_exists(dst_dir))
		rc = NB_ERR_EXISTS;
	if (rc == 0)
		rc = nb_file_mkdir(dst_dir, true, NULL);
	if (rc == 0)
		rc = ((file_status_10 =
			       nb_path_concat(&file_value_10, src_dir, DYN_TOOL_META_FILE)) != NB_OK
			      ? file_status_10
		      : (file_status_10 = nb_str_copy_to(nb_buf_str(&file_value_10), src_meta,
							 sizeof(src_meta))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_10);
	if (rc == 0)
		rc = ((file_status_11 = nb_path_concat(&file_value_11, src_dir,
						       DYN_TOOL_SOURCE_FILE)) != NB_OK
			      ? file_status_11
		      : (file_status_11 = nb_str_copy_to(nb_buf_str(&file_value_11), src_js,
							 sizeof(src_js))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_11);
	if (rc == 0)
		rc = ((file_status_12 =
			       nb_path_concat(&file_value_12, dst_dir, DYN_TOOL_META_FILE)) != NB_OK
			      ? file_status_12
		      : (file_status_12 = nb_str_copy_to(nb_buf_str(&file_value_12), dst_meta,
							 sizeof(dst_meta))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_12);
	if (rc == 0)
		rc = ((file_status_13 = nb_path_concat(&file_value_13, dst_dir,
						       DYN_TOOL_SOURCE_FILE)) != NB_OK
			      ? file_status_13
		      : (file_status_13 = nb_str_copy_to(nb_buf_str(&file_value_13), dst_js,
							 sizeof(dst_js))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_13);
	if (rc == 0) {
		rc = dynamic_tool_store_copy_tool_snapshot(src_meta, src_js, dst_meta, dst_js);
	}
	if (rc == 0)
		rc = load_tool_from_dir(ctx->reg, ctx->tctx, ctx->cfg, dst_dir,
					TOOL_ORIGIN_DYNAMIC_PERSISTENT);
	cJSON_Delete(root);
	if (rc < 0)
		return tool_result_error_format(
			result, "tool_failed", "failed to promote dynamic tool: %s",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	return tool_result_success_format(result, "{\"name\":\"%s\",\"status\":\"promoted\"}",
					  name);
}

static int service_delete(const char *args_json, struct tool_result *result, void *user_data)
{
	nb_buf file_value_14 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_14 = NB_OK;

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
		return NB_ERR_INVAL;
	root = args_json ? cJSON_Parse(args_json) : NULL;
	if (!root)
		return tool_result_error_copy(result, "tool_failed", "invalid JSON");
	name_item = cJSON_GetObjectItem(root, "name");
	if (!cJSON_IsString(name_item) || !valid_tool_name(name_item->valuestring)) {
		cJSON_Delete(root);
		return tool_result_error_copy(result, "tool_failed",
					      "tool_delete requires valid name");
	}
	(void)nb_str_copy_to(
		nb_str_from_parts(name_item->valuestring,
				  nb_utf8_clamp_bytes(name_item->valuestring, sizeof(name) - 1)),
		name, (sizeof(name) - 1) + 1);
	name[sizeof(name) - 1] = '\0';
	entry = tool_borrow(ctx->reg, name);
	if (!entry) {
		cJSON_Delete(root);
		return tool_result_error_copy(result, "tool_failed", "dynamic tool not found");
	}
	if (!(entry->flags & TOOL_FLAG_DYNAMIC) ||
	    (entry->origin != TOOL_ORIGIN_DYNAMIC_SESSION &&
	     entry->origin != TOOL_ORIGIN_DYNAMIC_PERSISTENT)) {
		cJSON_Delete(root);
		return tool_result_error_copy(result, "tool_failed",
					      "tool_delete can only delete dynamic tools");
	}
	dt = entry->user_data;
	if (!dt || !dt->dir[0]) {
		cJSON_Delete(root);
		return tool_result_error_copy(result, "tool_failed",
					      "dynamic tool metadata missing");
	}
	origin = entry->origin;
	(void)nb_str_copy_to(
		nb_str_from_parts(dt->dir, nb_utf8_clamp_bytes(dt->dir, sizeof(dir) - 1)), dir,
		(sizeof(dir) - 1) + 1);
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
			return tool_result_error_copy(result, "tool_failed",
						      "dynamic tool deletion denied");
		}
	}
	if (!dynamic_tool_delete_path_allowed(ctx, origin, name, dir)) {
		cJSON_Delete(root);
		return tool_result_error_copy(result, "tool_failed",
					      "dynamic tool deletion refused: path is outside the "
					      "expected dynamic tools directory");
	}
	rc = dynamic_tool_store_remove_tree(dir);
	if (rc == NB_ERR_NOT_FOUND)
		rc = 0;
	if (rc == 0)
		rc = tool_unregister(ctx->reg, name);
	if (rc == 0 && origin == TOOL_ORIGIN_DYNAMIC_SESSION) {
		char persistent_dir[PATH_MAX];
		int load_rc;

		load_rc = ((file_status_14 = nb_path_concat(
				    &file_value_14, ctx->cfg->persistent_dir, name)) != NB_OK
				   ? file_status_14
			   : (file_status_14 =
				      nb_str_copy_to(nb_buf_str(&file_value_14), persistent_dir,
						     sizeof(persistent_dir))) == NB_ERR_RANGE
				   ? NB_ERR_NAME_TOO_LONG
				   : file_status_14);
		if (load_rc == 0 && nb_file_exists(persistent_dir)) {
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
		return tool_result_error_format(
			result, "tool_failed", "failed to delete dynamic tool: %s",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	return tool_result_success_format(result, "{\"name\":\"%s\",\"status\":\"deleted\"}", name);
}

static int parse_tool_name_arg(const char *args_json, const char *tool_name, cJSON **root_out,
			       char name[TOOL_NAME_MAX])
{
	cJSON *root;
	cJSON *name_item;

	if (!root_out || !name)
		return NB_ERR_INVAL;
	*root_out = NULL;
	root = args_json ? cJSON_Parse(args_json) : NULL;
	if (!root)
		return NB_ERR_INVAL;
	name_item = cJSON_GetObjectItem(root, "name");
	if (!cJSON_IsString(name_item) || !valid_tool_name(name_item->valuestring)) {
		cJSON_Delete(root);
		return NB_ERR_INVAL;
	}
	(void)nb_str_copy_to(
		nb_str_from_parts(name_item->valuestring,
				  nb_utf8_clamp_bytes(name_item->valuestring, TOOL_NAME_MAX - 1)),
		name, (TOOL_NAME_MAX - 1) + 1);
	name[TOOL_NAME_MAX - 1] = '\0';
	*root_out = root;
	(void)tool_name;
	return 0;
}

static int result_take_cjson(struct tool_result *result, cJSON **root)
{
	return tool_result_success_take(result, root);
}

static int service_history(const char *args_json, struct tool_result *result, void *user_data)
{
	int directory_status_1 = NB_OK;

	nb_buf file_value_15 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_15 = NB_OK;

	struct dynamic_tools_context *ctx = user_data;
	cJSON *args = NULL;
	cJSON *limit_item;
	cJSON *out NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	cJSON *items;
	char name[TOOL_NAME_MAX];
	char tool_dir[PATH_MAX];
	char history[PATH_MAX];
	nb_dir_listing dirs NB_CLEANUP(nb_dir_listing_destroy) = {0};
	int count = 0;
	int limit = 20;
	int rc;

	if (!ctx || !result)
		return NB_ERR_INVAL;
	rc = parse_tool_name_arg(args_json, "tool_history", &args, name);
	if (rc < 0)
		return tool_result_error_copy(result, "tool_failed",
					      "tool_history requires valid name");
	limit_item = cJSON_GetObjectItem(args, "limit");
	if (cJSON_IsNumber(limit_item) && limit_item->valueint > 0)
		limit = limit_item->valueint;
	cJSON_Delete(args);
	rc = create_context_dir(ctx, name, tool_dir, sizeof(tool_dir));
	if (rc == 0)
		rc = dynamic_tool_store_history_dir_path(tool_dir, history, sizeof(history));
	if (rc < 0)
		return tool_result_error_format(
			result, "tool_failed", "failed to read tool history: %s",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	out = cJSON_CreateObject();
	items = cJSON_CreateArray();
	if (!out || !items) {
		cJSON_Delete(items);
		return NB_ERR_NOMEM;
	}
	cJSON_AddStringToObject(out, "name", name);
	cJSON_AddItemToObject(out, "checkpoints", items);
	if (nb_file_exists(history) &&
	    ((directory_status_1 = nb_dir_scan(
		      history, NB_DIR_SCAN_DIRS | NB_DIR_SCAN_HIDDEN | NB_DIR_SCAN_FOLLOW_LINKS,
		      &dirs, NULL)) != NB_OK
		     ? directory_status_1
	     : dirs.names.len > INT_MAX ? NB_ERR_RANGE
					: (count = (int)dirs.names.len, NB_OK)) == 0) {
		int emitted = 0;

		while (emitted < limit) {
			int max_idx = -1;
			int max_id = -1;

			for (int i = 0; i < count; i++) {
				int value = dynamic_tool_store_checkpoint_id_value(
					nb_dir_listing_name(&dirs, (size_t)(i)));
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

				if (((file_status_15 = nb_path_concat(
					      &file_value_15, history,
					      nb_dir_listing_name(&dirs, (size_t)(max_idx)))) !=
						     NB_OK
					     ? file_status_15
				     : (file_status_15 = nb_str_copy_to(nb_buf_str(&file_value_15),
									cp_dir, sizeof(cp_dir))) ==
						     NB_ERR_RANGE
					     ? NB_ERR_NAME_TOO_LONG
					     : file_status_15) == 0 &&
				    dynamic_tool_store_read_checkpoint_meta(cp_dir, &cp) == 0) {
					item = cJSON_CreateObject();
					if (!item) {
						nb_dir_listing_destroy(&dirs);
						return NB_ERR_NOMEM;
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
			nb_buf_clear(nb_vec_at(&dirs.names, (size_t)max_idx));
		}
		nb_dir_listing_destroy(&dirs);
	}
	return result_take_cjson(result, &out);
}

static int service_diff(const char *args_json, struct tool_result *result, void *user_data)
{
	struct dynamic_tools_context *ctx = user_data;
	struct tool_entry *entry;
	struct dynamic_tool *dt;
	struct tool_checkpoint cp;
	cJSON *args = NULL;
	cJSON *id_item;
	cJSON *out NB_CLEANUP(cJSON_DeleteOwner) = NULL;
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
	nb_buf diff NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc;

	if (!ctx || !result)
		return NB_ERR_INVAL;
	rc = parse_tool_name_arg(args_json, "tool_diff", &args, name);
	if (rc < 0)
		return tool_result_error_copy(result, "tool_failed",
					      "tool_diff requires valid name");
	id_item = cJSON_GetObjectItem(args, "checkpoint_id");
	if (cJSON_IsString(id_item) && id_item->valuestring &&
	    strcmp(id_item->valuestring, "latest") != 0) {
		(void)nb_str_copy_to(nb_str_from_parts(id_item->valuestring,
						       nb_utf8_clamp_bytes(id_item->valuestring,
									   sizeof(cp_id) - 1)),
				     cp_id, (sizeof(cp_id) - 1) + 1);
		cp_id[sizeof(cp_id) - 1] = '\0';
	} else {
		char tool_dir[PATH_MAX];
		rc = create_context_dir(ctx, name, tool_dir, sizeof(tool_dir));
		if (rc == 0)
			rc = dynamic_tool_store_latest_checkpoint_id(tool_dir, cp_id,
								     sizeof(cp_id));
		if (rc < 0) {
			cJSON_Delete(args);
			return tool_result_error_copy(result, "tool_failed",
						      "checkpoint not found");
		}
	}
	cJSON_Delete(args);
	entry = tool_borrow(ctx->reg, name);
	if (!entry || !(entry->flags & TOOL_FLAG_DYNAMIC) || !entry->user_data)
		return tool_result_error_copy(result, "tool_failed", "dynamic tool not found");
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
		return tool_result_error_copy(result, "tool_failed", "checkpoint not found");
	old_meta = dynamic_tool_store_optional_text_create(old_meta_path);
	old_source = dynamic_tool_store_optional_text_create(old_source_path);
	new_meta = dynamic_tool_store_optional_text_create(new_meta_path);
	new_source = dynamic_tool_store_optional_text_create(new_source_path);
	if (!old_meta || !old_source || !new_meta || !new_source) {
		free(old_meta);
		free(old_source);
		free(new_meta);
		free(new_source);
		return NB_ERR_NOMEM;
	}
	rc = (nb_buf_init(&diff), nb_buf_reserve(&diff, 4096) ? 0 : NB_ERR_NOMEM);
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
		nb_buf_cleanup(&diff);
		return rc;
	}
	out = cJSON_CreateObject();
	if (!out) {
		nb_buf_cleanup(&diff);
		return NB_ERR_NOMEM;
	}
	cJSON_AddStringToObject(out, "name", name);
	cJSON_AddStringToObject(out, "checkpoint_id", cp.id);
	cJSON_AddStringToObject(out, "before_state", cp.before_state);
	cJSON_AddStringToObject(out, "diff", nb_buf_cstr(&diff));
	nb_buf_cleanup(&diff);
	return result_take_cjson(result, &out);
}

static int reload_persistent_if_present(struct dynamic_tools_context *ctx, const char *name)
{
	nb_buf file_value_16 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_16 = NB_OK;

	char persistent_dir[PATH_MAX];
	int rc;

	rc = ((file_status_16 = nb_path_concat(&file_value_16, ctx->cfg->persistent_dir, name)) !=
			      NB_OK
		      ? file_status_16
	      : (file_status_16 = nb_str_copy_to(nb_buf_str(&file_value_16), persistent_dir,
						 sizeof(persistent_dir))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_16);
	if (rc < 0)
		return rc;
	if (!nb_file_exists(persistent_dir))
		return 0;
	rc = load_tool_from_dir(ctx->reg, ctx->tctx, ctx->cfg, persistent_dir,
				TOOL_ORIGIN_DYNAMIC_PERSISTENT);
	return rc < 0 ? rc : 0;
}

static int service_rollback(const char *args_json, struct tool_result *result, void *user_data)
{
	nb_buf file_value_17 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_17 = NB_OK;

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
		return NB_ERR_INVAL;
	rc = parse_tool_name_arg(args_json, "tool_rollback", &args, name);
	if (rc < 0)
		return tool_result_error_copy(result, "tool_failed",
					      "tool_rollback requires valid name");
	id_item = cJSON_GetObjectItem(args, "checkpoint_id");
	if (cJSON_IsString(id_item) && id_item->valuestring &&
	    strcmp(id_item->valuestring, "latest") != 0) {
		(void)nb_str_copy_to(nb_str_from_parts(id_item->valuestring,
						       nb_utf8_clamp_bytes(id_item->valuestring,
									   sizeof(cp_id) - 1)),
				     cp_id, (sizeof(cp_id) - 1) + 1);
		cp_id[sizeof(cp_id) - 1] = '\0';
	} else {
		rc = create_context_dir(ctx, name, tool_dir, sizeof(tool_dir));
		if (rc == 0)
			rc = dynamic_tool_store_latest_checkpoint_id(tool_dir, cp_id,
								     sizeof(cp_id));
		if (rc < 0) {
			cJSON_Delete(args);
			return tool_result_error_copy(result, "tool_failed",
						      "checkpoint not found");
		}
	}
	cJSON_Delete(args);
	entry = tool_borrow(ctx->reg, name);
	if (!entry || !(entry->flags & TOOL_FLAG_DYNAMIC) || !entry->user_data)
		return tool_result_error_copy(result, "tool_failed", "dynamic tool not found");
	dt = entry->user_data;
	(void)nb_str_copy_to(
		nb_str_from_parts(dt->dir, nb_utf8_clamp_bytes(dt->dir, sizeof(tool_dir) - 1)),
		tool_dir, (sizeof(tool_dir) - 1) + 1);
	tool_dir[sizeof(tool_dir) - 1] = '\0';
	rc = dynamic_tool_store_checkpoint_dir_path(tool_dir, cp_id, cp_dir, sizeof(cp_dir));
	if (rc == 0)
		rc = dynamic_tool_store_read_checkpoint_meta(cp_dir, &cp);
	if (rc < 0)
		return tool_result_error_copy(result, "tool_failed", "checkpoint not found");
	rc = dynamic_tool_store_restore_tool_checkpoint(tool_dir, cp_dir, &cp);
	if (rc < 0)
		return tool_result_error_format(
			result, "tool_failed", "failed to restore checkpoint: %s",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	if (strcmp(cp.before_state, "absent") == 0) {
		rc = tool_unregister(ctx->reg, name);
		if (rc == NB_ERR_NOT_FOUND)
			rc = 0;
		if (rc == 0)
			rc = reload_persistent_if_present(ctx, name);
		if (rc < 0)
			return tool_result_error_format(
				result, "tool_failed", "failed to unregister rolled back tool: %s",
				nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return tool_result_success_format(result,
						  "{\"name\":\"%s\",\"status\":\"rolled_back\","
						  "\"checkpoint_id\":\"%s\",\"state\":\"absent\"}",
						  name, cp.id);
	}
	rc = ((file_status_17 = nb_path_concat(&file_value_17, tool_dir, DYN_TOOL_SOURCE_FILE)) !=
			      NB_OK
		      ? file_status_17
	      : (file_status_17 = nb_str_copy_to(nb_buf_str(&file_value_17), source_path,
						 sizeof(source_path))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_17);
	if (rc == 0)
		rc = dynamic_tool_runner_check_js_source(source_path, &check_error);
	if (rc < 0) {
		int out_rc;
		out_rc = tool_result_error_format(
			result, "tool_failed", "restored checkpoint failed validation: %s",
			check_error
				? check_error
				: nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		free(check_error);
		return out_rc;
	}
	free(check_error);
	rc = load_tool_from_dir(ctx->reg, ctx->tctx, ctx->cfg, tool_dir,
				TOOL_ORIGIN_DYNAMIC_SESSION);
	if (rc < 0)
		return tool_result_error_format(
			result, "tool_failed", "failed to register rolled back tool: %s",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	(void)dynamic_tool_store_remove_checkpoint_ids_from(
		tool_dir, dynamic_tool_store_checkpoint_id_value(cp.id));
	return tool_result_success_format(result,
					  "{\"name\":\"%s\",\"status\":\"rolled_back\","
					  "\"checkpoint_id\":\"%s\",\"state\":\"present\"}",
					  name, cp.id);
}

int dynamic_tool_service_load_root(struct tool_registry *reg, struct tool_context *tctx,
				   const struct config_dynamic_tools *cfg, const char *root,
				   enum tool_origin origin)
{
	int directory_status_1 = NB_OK;

	nb_buf file_value_18 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_18 = NB_OK;

	nb_dir_listing dirs NB_CLEANUP(nb_dir_listing_destroy) = {0};
	int count = 0;

	if (!reg || !cfg || !cfg->enabled || !root)
		return 0;
	if (!nb_file_exists(root))
		return 0;
	if (((directory_status_1 = nb_dir_scan(
		      root, NB_DIR_SCAN_DIRS | NB_DIR_SCAN_HIDDEN | NB_DIR_SCAN_FOLLOW_LINKS, &dirs,
		      NULL)) != NB_OK
		     ? directory_status_1
	     : dirs.names.len > INT_MAX ? NB_ERR_RANGE
					: (count = (int)dirs.names.len, NB_OK)) != 0)
		return 0;
	for (int i = 0; i < count; i++) {
		char path[PATH_MAX];
		if (nb_dir_listing_name(&dirs, (size_t)(i))[0] == '.')
			continue;
		if (((file_status_18 = nb_path_concat(&file_value_18, root,
						      nb_dir_listing_name(&dirs, (size_t)(i)))) !=
				     NB_OK
			     ? file_status_18
		     : (file_status_18 = nb_str_copy_to(nb_buf_str(&file_value_18), path,
							sizeof(path))) == NB_ERR_RANGE
			     ? NB_ERR_NAME_TOO_LONG
			     : file_status_18) != 0)
			continue;
		(void)load_tool_from_dir(reg, tctx, cfg, path, origin);
	}
	nb_dir_listing_destroy(&dirs);
	return 0;
}

int dynamic_tool_service_load_session(struct tool_registry *reg, struct tool_context *tctx,
				      const struct config_dynamic_tools *cfg,
				      const char *session_id)
{
	nb_buf file_value_19 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_19 = NB_OK;

	char session_root[PATH_MAX];
	int rc;

	if (!cfg || !cfg->enabled || !session_id || !*session_id)
		return 0;
	rc = ((file_status_19 = nb_path_concat(&file_value_19, cfg->session_dir, session_id)) !=
			      NB_OK
		      ? file_status_19
	      : (file_status_19 = nb_str_copy_to(nb_buf_str(&file_value_19), session_root,
						 sizeof(session_root))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_19);
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
		return NB_ERR_INVAL;
	nb_mutex_lock(context->reg->mutation_mutex);
	if (dynamic_tool_runner_cancelled(result)) {
		nb_mutex_unlock(context->reg->mutation_mutex);
		return NB_ERR_CANCELLED;
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
		rc = NB_ERR_INVAL;
		break;
	}
	nb_mutex_unlock(context->reg->mutation_mutex);
	return rc;
}
