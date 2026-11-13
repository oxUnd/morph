#include "runtime/extensions.h"
#include "nb/str.h"
#include "nb/utf8.h"
#include <limits.h>
#include "nb/err.h"
#include "cJSON.h"
#include "nb/cleanup.h"

#ifdef MORPH_NO_SHELL

#include <errno.h>

int runtime_extensions_load(struct runtime_context *ctx, const char *front_name)
{
	(void)ctx;
	(void)front_name;
	return NB_ERR_UNSUPPORTED;
}

#else

#include "runtime/context_owner.h"
#include "agent/guardrail.h"
#include "ext/ext.h"
#include "agent/tool_runtime.h"
#include "nb/file.h"
#include "permission/context.h"
#include "sandbox/permission.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int runtime_ext_cancelled(void *opaque)
{
	const struct tool_result *result = opaque;
	return result->runtime != NULL && http_wait_cancelable(&result->runtime->http, 0) != 0;
}

static int runtime_ext_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct ext *extension = user_data;
	char *output NB_CLEANUP(nb_charp_cleanup) = NULL;
	int rc;

	if (!extension)
		return NB_ERR_INVAL;
	struct tool_context *context = result->runtime ? result->runtime->permission_context : NULL;
	struct sandbox_config limits NB_CLEANUP(sandbox_config_cleanup) = {0};
	struct permission_policy policy = {0};

	if (context) {
		(void)tool_context_get_permission_policy(context, &policy);
		struct tool_operation operation = {
			.kind = TOOL_OP_TOOL_CALL,
			.tool_name = extension->manifest.name,
			.principal = extension->manifest.name,
			.action = args_json,
			.details_json = args_json,
			.scope = extension->path,
			.network = !!(extension->manifest.permissions & EXT_PERM_NETWORK),
		};
		rc = tool_context_check_operation(context, &operation);
		if (rc == 0)
			rc = tool_context_add_builtin_path(context, TOOL_PATH_READ,
							   extension->path);
		if (rc == 0)
			rc = sandbox_permission_prepare(context, extension->manifest.name, NULL,
							&limits);
		limits.network_access = operation.network;
	} else {
		rc = NB_ERR_NOT_PERMITTED;
	}
	if (rc == 0)
		rc = ext_run_bounded(extension, args_json, &output, runtime_ext_cancelled, result,
				     permission_effective_access(&policy).host ? NULL : &limits);
	if (output)
		(void)tool_result_success_json_text_copy(result, output);
	return rc;
}

static void runtime_ext_destroy(void *user_data)
{
	struct ext *extension = user_data;

	if (!extension)
		return;
	ext_unload(extension);
	free(extension);
}

static void runtime_ext_move(struct ext *destination, struct ext *source)
{
	*destination = *source;
	memset(source, 0, sizeof(*source));
}

static void runtime_register_guardrail_extension(struct runtime_context *ctx, struct ext *extension,
						 const char *path)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;

	enum guardrail_hook hook = GUARDRAIL_HOOK_OUTPUT;
	struct guardrail_rule *rule;
	char entry[PATH_MAX];

	if (strcmp(extension->manifest.hook, "input") == 0)
		hook = GUARDRAIL_HOOK_INPUT;
	else if (strcmp(extension->manifest.hook, "tool_output") == 0)
		hook = GUARDRAIL_HOOK_TOOL_OUTPUT;
	guardrail_rule_register(&ctx->react->guardrail, extension->manifest.name, hook,
				GUARDRAIL_RULE_EXT, NULL, extension->manifest.description, NULL,
				extension->manifest.action_text[0] ? extension->manifest.action_text
								   : NULL);
	rule = guardrail_rule_borrow(&ctx->react->guardrail, extension->manifest.name);
	if (!rule ||
	    ((file_status_1 = nb_path_concat(&file_value_1, path, extension->manifest.entry)) !=
			     NB_OK
		     ? file_status_1
	     : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), entry, sizeof(entry))) ==
			     NB_ERR_RANGE
		     ? NB_ERR_NAME_TOO_LONG
		     : file_status_1) != 0)
		return;
	(void)nb_str_copy_to(
		nb_str_from_parts(entry, nb_utf8_clamp_bytes(entry, sizeof(rule->ext_entry) - 1)),
		rule->ext_entry, (sizeof(rule->ext_entry) - 1) + 1);
	if (strcmp(extension->manifest.type, "so") == 0) {
		rule->ext_type = GUARDRAIL_EXT_SO;
		(void)guardrail_ext_so_load(ctx ? ctx->logger : NULL, rule);
	} else {
		rule->ext_type = GUARDRAIL_EXT_EXEC;
	}
}

int runtime_extensions_load(struct runtime_context *ctx, const char *front_name)
{
	int directory_status_1 = NB_OK;

	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_3 = NB_OK;

	char directory[PATH_MAX] = {0};
	nb_dir_listing names NB_CLEANUP(nb_dir_listing_destroy) = {0};
	char *expanded;
	int count = 0;
	int rc;

	if (!ctx)
		return NB_ERR_INVAL;
	expanded = (nb_path_expand(&file_value_2,
				   ctx->config.ext.dir[0] ? ctx->config.ext.dir : "~/.morph/exts",
				   NULL) == NB_OK
			    ? strdup(nb_buf_cstr(&file_value_2))
			    : NULL);
	(void)nb_str_copy_to(nb_str_from_parts(expanded ? expanded : "exts",
					       nb_utf8_clamp_bytes(expanded ? expanded : "exts",
								   sizeof(directory) - 1)),
			     directory, (sizeof(directory) - 1) + 1);
	free(expanded);
	if (!nb_file_exists(directory))
		(void)nb_file_mkdir(directory, true, NULL);
	rc = ((directory_status_1 = nb_dir_scan(
		       directory, NB_DIR_SCAN_DIRS | NB_DIR_SCAN_HIDDEN | NB_DIR_SCAN_FOLLOW_LINKS,
		       &names, NULL)) != NB_OK
		      ? directory_status_1
	      : names.names.len > INT_MAX ? NB_ERR_RANGE
					  : (count = (int)names.names.len, NB_OK));
	if (rc != 0)
		return rc;
	for (int i = 0; i < count; i++) {
		char path[PATH_MAX];
		struct ext loaded = {0};
		struct ext *owned;
		struct tool_spec spec;

		if (nb_dir_listing_name(&names, (size_t)(i))[0] == '.' ||
		    ((file_status_3 = nb_path_concat(&file_value_3, directory,
						     nb_dir_listing_name(&names, (size_t)(i)))) !=
				     NB_OK
			     ? file_status_3
		     : (file_status_3 = nb_str_copy_to(nb_buf_str(&file_value_3), path,
						       sizeof(path))) == NB_ERR_RANGE
			     ? NB_ERR_NAME_TOO_LONG
			     : file_status_3) != 0)
			continue;
		memset(&loaded, 0, sizeof(loaded));
		if (ext_load(ctx ? ctx->logger : NULL, &loaded, path) != 0 || !loaded.enabled) {
			ext_unload(&loaded);
			continue;
		}
		if (front_name && front_name[0] &&
		    !ext_manifest_supports_front(&loaded.manifest, front_name)) {
			ext_unload(&loaded);
			continue;
		}
		if (loaded.manifest.purpose == EXT_PURPOSE_GUARDRAIL) {
			runtime_register_guardrail_extension(ctx, &loaded, path);
			ext_unload(&loaded);
			continue;
		}
		owned = malloc(sizeof(*owned));
		if (!owned) {
			ext_unload(&loaded);
			rc = NB_ERR_NOMEM;
			break;
		}
		runtime_ext_move(owned, &loaded);
		memset(&spec, 0, sizeof(spec));
		spec.origin = TOOL_ORIGIN_EXT;
		spec.name = owned->manifest.name;
		spec.description = owned->manifest.description;
		spec.input_schema = owned->manifest.input_schema;
		spec.output_schema = owned->manifest.output_schema;
		spec.exec = runtime_ext_exec;
		spec.flags = TOOL_FLAG_INTERNAL_APPROVAL;
		spec.user_data = owned;
		spec.user_data_destroy = runtime_ext_destroy;
		if (tool_register_take(&ctx->tools, &spec) != 0)
			runtime_ext_destroy(owned);
	}
	nb_dir_listing_destroy(&names);
	return rc;
}

#endif
