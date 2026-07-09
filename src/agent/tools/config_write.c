#include "config_write.h"
#include "agent/tool_context.h"
#include "util/file.h"
#include "util/buf.h"
#include "util/error.h"
#include "cJSON.h"
#include "toml.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <libgen.h>

#define CONFIG_WRITE_MAX_SIZE (1024 * 1024)

struct config_write_context {
	struct tool_context *tctx;
	char config_path[PATH_MAX];
};

static void config_write_context_destroy(void *user_data)
{
	free(user_data);
}

static int path_equal_config(const char *path, const char *config_path)
{
	char *a;
	char *b;
	int same;

	if (!path || !config_path)
		return 0;
	a = file_expand_path(path);
	b = file_expand_path(config_path);
	if (!a || !b) {
		free(a);
		free(b);
		return 0;
	}
	same = strcmp(a, b) == 0;
	free(a);
	free(b);
	return same;
}

static int validate_toml_content(const char *content, char *errbuf,
				 size_t errbuf_size)
{
	char *copy;
	toml_table_t *tbl;

	if (!content)
		MORPH_RETURN(-EINVAL);
	copy = strdup(content);
	if (!copy)
		MORPH_RETURN(-ENOMEM);
	if (errbuf_size > 0)
		errbuf[0] = '\0';
	tbl = toml_parse(copy, errbuf, (int)errbuf_size);
	free(copy);
	if (!tbl)
		MORPH_RETURN(MORPH_ERR_PARSE);
	toml_free(tbl);
	return 0;
}

static int append_toml_string(morph_buf_t *buf, const char *s)
{
	int rc;

	rc = morph_buf_putc(buf, '"');
	if (rc != 0)
		return rc;
	for (const char *p = s ? s : ""; *p; p++) {
		switch (*p) {
		case '\\':
			rc = morph_buf_puts(buf, "\\\\");
			break;
		case '"':
			rc = morph_buf_puts(buf, "\\\"");
			break;
		case '\n':
			rc = morph_buf_puts(buf, "\\n");
			break;
		case '\r':
			rc = morph_buf_puts(buf, "\\r");
			break;
		case '\t':
			rc = morph_buf_puts(buf, "\\t");
			break;
		default:
			rc = morph_buf_putc(buf, *p);
			break;
		}
		if (rc != 0)
			return rc;
	}
	return morph_buf_putc(buf, '"');
}

static int append_patch_value(morph_buf_t *buf, cJSON *value)
{
	if (cJSON_IsString(value))
		return append_toml_string(buf, value->valuestring);
	if (cJSON_IsBool(value))
		return morph_buf_puts(buf, cJSON_IsTrue(value) ? "true" : "false");
	if (cJSON_IsNumber(value)) {
		if ((double)value->valueint == value->valuedouble)
			return morph_buf_printf(buf, "%d", value->valueint);
		return morph_buf_printf(buf, "%.17g", value->valuedouble);
	}
	MORPH_RETURN(-EINVAL);
}

static int build_content_from_patches(const char *old_content, cJSON *patches,
				      char **out)
{
	morph_buf_t buf;
	int rc;

	if (!cJSON_IsArray(patches) || !out)
		MORPH_RETURN(-EINVAL);
	*out = NULL;
	rc = morph_buf_init(&buf, old_content ? strlen(old_content) + 1024 : 4096);
	if (rc != 0)
		return rc;
	if (old_content && *old_content) {
		rc = morph_buf_puts(&buf, old_content);
		if (rc != 0)
			goto out;
		if (buf.len > 0 && buf.data[buf.len - 1] != '\n') {
			rc = morph_buf_putc(&buf, '\n');
			if (rc != 0)
				goto out;
		}
	}
	rc = morph_buf_puts(&buf, "\n# Added by morph config_write\n");
	if (rc != 0)
		goto out;
	cJSON *patch = NULL;
	cJSON_ArrayForEach(patch, patches) {
		cJSON *section = cJSON_GetObjectItem(patch, "section");
		cJSON *key = cJSON_GetObjectItem(patch, "key");
		cJSON *value = cJSON_GetObjectItem(patch, "value");
		if (!cJSON_IsString(section) || !section->valuestring[0] ||
		    !cJSON_IsString(key) || !key->valuestring[0] || !value) {
			rc = -EINVAL;
			goto out;
		}
		rc = morph_buf_printf(&buf, "[%s]\n%s = ",
				      section->valuestring, key->valuestring);
		if (rc != 0)
			goto out;
		rc = append_patch_value(&buf, value);
		if (rc != 0)
			goto out;
		rc = morph_buf_puts(&buf, "\n");
		if (rc != 0)
			goto out;
	}
	*out = morph_buf_detach(&buf);
	if (!*out)
		rc = -ENOMEM;
	return rc;

out:
	morph_buf_cleanup(&buf);
	return rc;
}

static int ensure_parent_dir(const char *path)
{
	char tmp[PATH_MAX];
	char *dir;

	if (!path)
		MORPH_RETURN(-EINVAL);
	strncpy(tmp, path, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';
	dir = dirname(tmp);
	if (!dir || !*dir)
		return 0;
	return file_ensure_dir(dir);
}

static int config_write_exec(const char *args_json, struct tool_result *result,
			     void *user_data)
{
	struct config_write_context *ctx = user_data;
	cJSON *root = NULL;
	char *target_expanded = NULL;
	char *old_content = NULL;
	char *new_content = NULL;
	const char *path;
	const char *reason;
	char errbuf[256];
	int rc = 0;

	if (!result)
		MORPH_RETURN(-EINVAL);
	if (!ctx || !ctx->config_path[0]) {
		(void)tool_result_error(result, "tool_failed",
					     "config_write is not configured");
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	}
	root = cJSON_Parse(args_json);
	if (!root) {
		(void)tool_result_error(result, "tool_failed", "invalid JSON");
		MORPH_RETURN(-EINVAL);
	}
	cJSON *path_item = cJSON_GetObjectItem(root, "path");
	cJSON *content_item = cJSON_GetObjectItem(root, "content");
	cJSON *patches = cJSON_GetObjectItem(root, "patches");
	cJSON *reason_item = cJSON_GetObjectItem(root, "reason");
	path = cJSON_IsString(path_item) && path_item->valuestring[0] ?
		path_item->valuestring : ctx->config_path;
	reason = cJSON_IsString(reason_item) ? reason_item->valuestring : "";
	if (!reason[0]) {
		rc = tool_result_error(result, "tool_failed", "missing 'reason' parameter");
		rc = rc != 0 ? rc : -EINVAL;
		goto out;
	}
	if (!path_equal_config(path, ctx->config_path)) {
		rc = tool_result_error(result, "tool_failed",
			"config_write can only write the active Morph config file");
		rc = rc != 0 ? rc : -EACCES;
		goto out;
	}
	target_expanded = file_expand_path(ctx->config_path);
	if (!target_expanded) {
		rc = -ENOMEM;
		goto out;
	}
	if (cJSON_IsString(content_item) && content_item->valuestring) {
		if (strlen(content_item->valuestring) > CONFIG_WRITE_MAX_SIZE) {
			rc = tool_result_error(result, "tool_failed", "content too large");
			rc = rc != 0 ? rc : -EFBIG;
			goto out;
		}
		new_content = strdup(content_item->valuestring);
		if (!new_content) {
			rc = -ENOMEM;
			goto out;
		}
	} else if (cJSON_IsArray(patches)) {
		old_content = file_read_all(target_expanded, NULL);
		rc = build_content_from_patches(old_content, patches, &new_content);
		if (rc < 0) {
			(void)tool_result_error(result, "tool_failed",
				"invalid patches; use section/key/value entries");
			goto out;
		}
		if (strlen(new_content) > CONFIG_WRITE_MAX_SIZE) {
			rc = tool_result_error(result, "tool_failed", "content too large");
			rc = rc != 0 ? rc : -EFBIG;
			goto out;
		}
	} else {
		rc = tool_result_error(result, "tool_failed",
			"provide either 'content' or 'patches'");
		rc = rc != 0 ? rc : -EINVAL;
		goto out;
	}
	rc = validate_toml_content(new_content, errbuf, sizeof(errbuf));
	if (rc < 0) {
		(void)tool_result_errorf(result, "tool_failed",
					      "invalid TOML: %s", errbuf);
		goto out;
	}
	if (ctx->tctx) {
		struct tool_operation op = {
			.kind = TOOL_OP_PATH_WRITE,
			.tool_name = "config_write",
			.action = reason,
			.target = target_expanded,
			.scope = "active Morph config file",
			.details_json = NULL,
		};
		rc = tool_context_check_operation(ctx->tctx, &op);
		if (rc < 0) {
			(void)tool_result_error(result, "tool_failed",
				"config write denied or not approved");
			goto out;
		}
	}
	rc = ensure_parent_dir(target_expanded);
	if (rc < 0)
		goto out;
	rc = file_write_all(target_expanded, new_content, strlen(new_content));
	if (rc < 0)
		goto out;
	{
		cJSON *out_json = cJSON_CreateObject();
		char *out_str;
		if (!out_json) {
			rc = -ENOMEM;
			goto out;
		}
		cJSON_AddStringToObject(out_json, "path", target_expanded);
		cJSON_AddBoolToObject(out_json, "changed", 1);
		cJSON_AddBoolToObject(out_json, "validated", 1);
		cJSON_AddStringToObject(out_json, "message",
			"configuration written; restart Morph to reload startup settings");
		out_str = cJSON_PrintUnformatted(out_json);
		cJSON_Delete(out_json);
		(void)tool_result_success_json_text(result, out_str);
	}

out:
	free(target_expanded);
	free(old_content);
	free(new_content);
	cJSON_Delete(root);
	return rc;
}

int config_write_init(struct tool_registry *reg, struct tool_context *tctx,
		      const char *config_path)
{
	struct config_write_context *ctx;
	int rc;

	if (!reg || !config_path || !config_path[0])
		MORPH_RETURN(-EINVAL);
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		MORPH_RETURN(-ENOMEM);
	ctx->tctx = tctx;
	strncpy(ctx->config_path, config_path, sizeof(ctx->config_path) - 1);
	rc = tool_register(reg, &(struct tool_spec){ .origin = TOOL_ORIGIN_BUILTIN, .name = "config_write", .description = "Write the active Morph config file after user approval and TOML validation. "
		"Use only when the user asks to change Morph configuration. "
		"Prefer api_key_env over writing API key values. "
		"Args: path (optional, must be the active config path), reason (required), "
		"content (complete TOML) or patches (array of section/key/value entries).", .input_schema = "{\"type\":\"object\",\"properties\":{"
		"\"path\":{\"type\":\"string\",\"description\":\"Optional config path; must match the active Morph config\"},"
		"\"reason\":{\"type\":\"string\",\"description\":\"Human-readable reason shown for approval\"},"
		"\"content\":{\"type\":\"string\",\"description\":\"Complete TOML content to write\"},"
		"\"patches\":{\"type\":\"array\",\"description\":\"Simple append-only section/key/value patches\","
		"\"items\":{\"type\":\"object\",\"properties\":{"
		"\"section\":{\"type\":\"string\"},"
		"\"key\":{\"type\":\"string\"},"
		"\"value\":{}"
		"},\"required\":[\"section\",\"key\",\"value\"]}}"
		"},\"required\":[\"reason\"]}", .output_schema = TOOL_OBJECT_OUTPUT_SCHEMA, .exec = config_write_exec, .user_data = ctx, .user_data_destroy = config_write_context_destroy });
	if (rc < 0) {
		free(ctx);
		return rc;
	}
	{
		struct tool_entry *e = tool_lookup(reg, "config_write");
		if (e)
			e->flags |= TOOL_FLAG_INTERNAL_APPROVAL;
	}
	return 0;
}
