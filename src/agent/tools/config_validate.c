#include "config_validate.h"
#include "agent/tool_context.h"
#include "cJSON.h"
#include "config/config.h"
#include <nb/cleanup.h>
#include <nb/file.h>
#include <nb/str.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct config_validate_context {
	struct tool_context *tctx;
	char config_path[PATH_MAX];
};

static void config_validate_context_destroy(void *user_data)
{
	free(user_data);
}

struct config_validate_warnings {
	cJSON *items;
	int failed;
};

static void config_validate_warning(const struct config_validation_error *warning,
				    void *user_data)
{
	struct config_validate_warnings *warnings = user_data;
	cJSON *item NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!item || !cJSON_AddStringToObject(item, "path", warning->path) ||
	    !cJSON_AddStringToObject(item, "message", warning->message) ||
	    !cJSON_AddNumberToObject(item, "code", warning->code) ||
	    !cJSON_AddNumberToObject(item, "line", warning->line) ||
	    !cJSON_AddNumberToObject(item, "column", warning->column) ||
	    !cJSON_AddItemToArray(warnings->items, item)) {
		warnings->failed = 1;
		return;
	}

	item = NULL;
}

static int config_validate_exec(const char *args_json, struct tool_result *result,
			       void *user_data)
{
	struct config_validate_context *ctx = user_data;
	cJSON *args NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(args_json);
	cJSON *data NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	nb_buf expanded NB_CLEANUP(nb_buf_cleanup) = {0};
	struct config_validation_error error = {0};
	char resolved[PATH_MAX];
	struct config_validate_warnings warnings = {0};
	int rc;

	if (!result || !ctx)
		return NB_ERR_INVAL;

	cJSON *path = cJSON_GetObjectItemCaseSensitive(args, "path");
	cJSON *content = cJSON_GetObjectItemCaseSensitive(args, "content");
	if (!cJSON_IsObject(args) || (path && !cJSON_IsString(path)) ||
	    (content && !cJSON_IsString(content)) || (path && content) ||
	    (path && !path->valuestring[0])) {
		rc = tool_result_error_copy(result, "invalid_arguments",
			"Provide either a nonempty path or complete TOML content, or neither.");
		return rc != NB_OK ? rc : NB_ERR_INVAL;
	}

	data = cJSON_CreateObject();
	if (!data)
		return NB_ERR_NOMEM;

	warnings.items = cJSON_AddArrayToObject(data, "warnings");
	if (!warnings.items)
		return NB_ERR_NOMEM;

	if (content) {
		rc = config_validate_text_with_warnings(content->valuestring, &error,
			config_validate_warning, &warnings);
	} else {
		const char *target = path ? path->valuestring : ctx->config_path;
		if (!target[0]) {
			rc = tool_result_error_copy(result, "invalid_arguments",
				"No active config path; provide path or content.");
			return rc != NB_OK ? rc : NB_ERR_INVAL;
		}

		if (ctx->tctx) {
			rc = tool_context_authorize_path(ctx->tctx, TOOL_PATH_READ,
				target, resolved, sizeof(resolved));
		} else {
			rc = nb_path_expand(&expanded, target, NULL);
			if (rc == NB_OK)
				rc = nb_str_copy_to(nb_buf_str(&expanded), resolved,
						    sizeof(resolved));
		}

		if (rc != NB_OK) {
			int report_rc = tool_result_error_copy(result, "read_denied",
				"Cannot access config file under the current read policy.");
			return report_rc != NB_OK ? report_rc : rc;
		}

		rc = config_validate_file_with_warnings(resolved, &error,
			config_validate_warning, &warnings);
	}

	if (rc != NB_OK && error.code == CONFIG_VALIDATION_NONE) {
		int report_rc = tool_result_error_copy(result, "validation_failed",
			"Cannot read or validate configuration.");
		return report_rc != NB_OK ? report_rc : rc;
	}

	if (warnings.failed || !cJSON_AddBoolToObject(data, "valid", rc == NB_OK))
		return NB_ERR_NOMEM;

	if (!content && !cJSON_AddStringToObject(data, "path", resolved))
		return NB_ERR_NOMEM;

	if (rc != NB_OK) {
		cJSON *diagnostic = cJSON_AddObjectToObject(data, "error");
		/* Parser diagnostics may contain source text, including credentials. */
		const char *message = error.code == CONFIG_VALIDATION_SYNTAX
			? "Invalid TOML or UTF-8; check the reported location."
			: error.message;
		if (!diagnostic ||
		    !cJSON_AddNumberToObject(diagnostic, "code", error.code) ||
		    !cJSON_AddStringToObject(diagnostic, "path", error.path) ||
		    !cJSON_AddNumberToObject(diagnostic, "line", error.line) ||
		    !cJSON_AddNumberToObject(diagnostic, "column", error.column) ||
		    !cJSON_AddStringToObject(diagnostic, "message", message))
			return NB_ERR_NOMEM;
	}

	return tool_result_success_take(result, &data);
}

int config_validate_init(struct tool_registry *reg, struct tool_context *tctx,
			 const char *config_path)
{
	struct tool_spec spec NB_CLEANUP(tool_spec_cleanup) = {
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "config_validate",
		.description = "Validate Morph configuration without modifying or applying it. "
			"Checks TOML syntax, field types, values and cross-field constraints. "
			"Provide path for any readable config file, content for complete TOML, "
			"or neither for the active config. Returns valid and the first error. "
			"Does not test network connections or credentials.",
		.input_schema = "{\"type\":\"object\",\"properties\":{"
			"\"path\":{\"type\":\"string\",\"minLength\":1},"
			"\"content\":{\"type\":\"string\"}},"
			"\"not\":{\"required\":[\"path\",\"content\"]},"
			"\"additionalProperties\":false}",
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.flags = TOOL_FLAG_READONLY,
		.exec = config_validate_exec,
		.user_data_destroy = config_validate_context_destroy
	};

	if (!reg)
		return NB_ERR_INVAL;

	struct config_validate_context *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NB_ERR_NOMEM;

	spec.user_data = ctx;
	ctx->tctx = tctx;
	if (config_path) {
		int rc = nb_str_copy_to(nb_str_from_cstr(config_path), ctx->config_path,
					sizeof(ctx->config_path));
		if (rc != NB_OK)
			return rc;
	}

	return tool_register_take(reg, &spec);
}
