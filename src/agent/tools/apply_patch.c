#include "apply_patch.h"
#include "agent/patch.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "cJSON.h"
#include "util/buf.h"
#include "util/error.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int apply_patch_exec(const char *input, struct tool_result *result,
			    void *user_data)
{
	struct tool_context *tool_context = user_data;
	struct patch_result patch_result;
	char error[512];
	cJSON *data;
	cJSON *files;
	int rc;

	if (!input || !result || !tool_context)
		MORPH_RETURN(-EINVAL);
	rc = patch_apply(tool_context_workdir(tool_context), input,
			 &patch_result, error, sizeof(error));
	if (rc != 0) {
		(void)tool_result_error(result, "invalid_patch",
			error[0] ? error : morph_strerror(rc));
		patch_result_cleanup(&patch_result);
		return rc;
	}
	data = cJSON_CreateObject();
	files = cJSON_CreateArray();
	if (!data || !files) {
		cJSON_Delete(data);
		cJSON_Delete(files);
		patch_result_cleanup(&patch_result);
		MORPH_RETURN(-ENOMEM);
	}
	cJSON_AddBoolToObject(data, "changed", 1);
	cJSON_AddStringToObject(data, "workspace_root",
		tool_context_workdir(tool_context));
	cJSON_AddItemToObject(data, "files", files);
	for (size_t i = 0; i < patch_result.changes.nelts; i++) {
		struct patch_change *change = morph_array_get(
			&patch_result.changes, i);
		cJSON *item = cJSON_CreateObject();

		if (!item) {
			cJSON_Delete(data);
			patch_result_cleanup(&patch_result);
			MORPH_RETURN(-ENOMEM);
		}
		cJSON_AddStringToObject(item, "path", change->path);
		cJSON_AddStringToObject(item, "action",
			patch_action_name(change->action));
		cJSON_AddNumberToObject(item, "added", change->added);
		cJSON_AddNumberToObject(item, "removed", change->removed);
		cJSON_AddItemToArray(files, item);
	}
	patch_result_cleanup(&patch_result);
	return tool_result_success(result, data);
}

int apply_patch_init(struct tool_registry *registry,
		     struct tool_context *tool_context)
{
	morph_buf_t description;
	const char *workspace_root;
	struct tool_spec spec;
	int rc;
	static const char *format =
		"{\"type\":\"grammar\",\"syntax\":\"lark\",\"definition\":\""
		"start: begin section+ end NEWLINE?\\n"
		"begin: \\\"*** Begin Patch\\\" NEWLINE\\n"
		"end: \\\"*** End Patch\\\"\\n"
		"section: add | update | delete\\n"
		"add: \\\"*** Add File: \\\" PATH NEWLINE ADD_LINE+\\n"
		"update: \\\"*** Update File: \\\" PATH NEWLINE move? hunk+\\n"
		"delete: \\\"*** Delete File: \\\" PATH NEWLINE?\\n"
		"move: \\\"*** Move to: \\\" PATH NEWLINE\\n"
		"hunk: (\\\"@@\\\" | \\\"@@ \\\" CONTEXT) NEWLINE HUNK_LINE+ EOF?\\n"
		"EOF: \\\"*** End of File\\\" NEWLINE?\\n"
		"PATH: /[^\\\\r\\\\n]+/\\n"
		"CONTEXT: /[^\\\\r\\\\n]+/\\n"
		"ADD_LINE: /[+][^\\\\r\\\\n]*/ NEWLINE\\n"
		"HUNK_LINE: /[ +\\\\-][^\\\\r\\\\n]*/ NEWLINE\\n"
		"%import common.NEWLINE\"}";

	if (!registry || !tool_context)
		MORPH_RETURN(-EINVAL);
	workspace_root = tool_context_workdir(tool_context);
	if (!workspace_root || !workspace_root[0])
		MORPH_RETURN(-EINVAL);
	rc = morph_buf_init(&description, 1024);
	if (rc != 0)
		return rc;
	rc = morph_buf_printf(&description,
		"Apply a stripped-down Codex patch to text files. The workspace "
		"root is '%s'. Every patch path is relative to this exact directory; "
		"do not use absolute paths and do not repeat the workspace directory "
		"in a path. Use the exact envelope '*** Begin Patch\\n*** Add File: "
		"path\\n+content\\n*** End Patch'. Prefer bare '@@' for Update "
		"File hunks. An optional anchor after '@@ ' must be one complete "
		"source line copied verbatim, never a descriptive label. Numeric "
		"unified-diff ranges are not part of the Codex patch grammar. Hunk lines "
		"must start with space, +, or -. Move, Update, and Delete File are "
		"supported. Keep each call below "
		"4 KiB and at most 80 changed lines. Split large files across "
		"multiple calls by replacing a unique continuation marker in each "
		"successive Update File patch.", workspace_root);
	if (rc != 0) {
		morph_buf_cleanup(&description);
		return rc;
	}
	memset(&spec, 0, sizeof(spec));
	spec = (struct tool_spec){
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "apply_patch",
		.title = "Apply patch",
		.description = morph_buf_cstr(&description),
		.input_kind = TOOL_INPUT_TEXT,
		.input_format = format,
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.exec = apply_patch_exec,
		.user_data = tool_context,
	};
	rc = tool_register(registry, &spec);
	morph_buf_cleanup(&description);
	return rc;
}
