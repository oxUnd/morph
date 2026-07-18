#include "runtime/extensions.h"

#ifdef MORPH_NO_SHELL

#include <errno.h>

int runtime_extensions_load(struct runtime_context *ctx,
			    const char *front_name)
{
	(void)ctx;
	(void)front_name;
	return -ENOTSUP;
}

#else

#include "runtime/context_owner.h"
#include "agent/guardrail.h"
#include "ext/ext.h"
#include "util/file.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int runtime_ext_exec(const char *args_json, struct tool_result *result,
			    void *user_data)
{
	struct ext *extension = user_data;
	char *output = NULL;
	int rc;

	if (!extension)
		return -EINVAL;
	rc = ext_run(extension, args_json, &output);
	if (output)
		(void)tool_result_success_json_text(result, output);
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

static void runtime_register_guardrail_extension(struct runtime_context *ctx,
						 struct ext *extension,
						 const char *path)
{
	enum guardrail_hook hook = GUARDRAIL_HOOK_OUTPUT;
	struct guardrail_rule *rule;
	char entry[PATH_MAX];

	if (strcmp(extension->manifest.hook, "input") == 0)
		hook = GUARDRAIL_HOOK_INPUT;
	else if (strcmp(extension->manifest.hook, "tool_output") == 0)
		hook = GUARDRAIL_HOOK_TOOL_OUTPUT;
	guardrail_rule_register(&ctx->react->guardrail,
		extension->manifest.name, hook, GUARDRAIL_RULE_EXT, NULL,
		extension->manifest.description, NULL,
		extension->manifest.action_text[0]
			? extension->manifest.action_text : NULL);
	rule = guardrail_rule_lookup(&ctx->react->guardrail,
				     extension->manifest.name);
	if (!rule || file_path_join(entry, sizeof(entry), path,
				extension->manifest.entry) != 0)
		return;
	strncpy(rule->ext_entry, entry, sizeof(rule->ext_entry) - 1);
	if (strcmp(extension->manifest.type, "so") == 0) {
		rule->ext_type = GUARDRAIL_EXT_SO;
		(void)guardrail_ext_so_load(rule);
	} else {
		rule->ext_type = GUARDRAIL_EXT_EXEC;
	}
}

int runtime_extensions_load(struct runtime_context *ctx,
			    const char *front_name)
{
	char directory[PATH_MAX] = {0};
	char **names = NULL;
	char *expanded;
	int count = 0;
	int rc;

	if (!ctx)
		return -EINVAL;
	expanded = file_expand_path(ctx->config.ext.dir[0]
		? ctx->config.ext.dir : "~/.morph/exts");
	strncpy(directory, expanded ? expanded : "exts", sizeof(directory) - 1);
	free(expanded);
	if (!file_exists(directory))
		(void)file_ensure_dir(directory);
	rc = file_list_dirs(directory, &names, &count);
	if (rc != 0)
		return rc;
	for (int i = 0; i < count; i++) {
		char path[PATH_MAX];
		struct ext loaded;
		struct ext *owned;
		struct tool_spec spec;

		if (names[i][0] == '.' ||
		    file_path_join(path, sizeof(path), directory, names[i]) != 0)
			continue;
		memset(&loaded, 0, sizeof(loaded));
		if (ext_load(&loaded, path) != 0 || !loaded.enabled) {
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
			rc = -ENOMEM;
			break;
		}
		*owned = loaded;
		memset(&spec, 0, sizeof(spec));
		spec.origin = TOOL_ORIGIN_EXT;
		spec.name = owned->manifest.name;
		spec.description = owned->manifest.description;
		spec.input_schema = owned->manifest.input_schema;
		spec.output_schema = owned->manifest.output_schema;
		spec.exec = runtime_ext_exec;
		spec.user_data = owned;
		spec.user_data_destroy = runtime_ext_destroy;
		if (tool_register(&ctx->tools, &spec) != 0)
			runtime_ext_destroy(owned);
	}
	file_free_list(names, count);
	return rc;
}

#endif
