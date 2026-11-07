#include "apply_patch.h"
#include "util/json.h"
#include "util/cleanup.h"
#include "agent/patch.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "cJSON.h"
#include "util/buf.h"
#include "util/error.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int authorize_patch_path(const char *path, int deletion, void *opaque)
{
	struct tool_context *context = opaque;
	struct tool_operation operation = {
		.kind = deletion ? TOOL_OP_PATH_DELETE : TOOL_OP_PATH_WRITE,
		.tool_name = "apply_patch",
		.principal = "tool:*",
		.target = path,
		.scope = tool_context_workdir_borrow(context),
	};
	return tool_context_check_operation(context, &operation);
}

static int apply_patch_exec(const char *input, struct tool_result *result,
			    void *user_data)
{
	struct tool_context *tool_context = user_data;
	struct patch_result patch_result MORPH_CLEANUP(patch_result_cleanup) = {0};
	cJSON *data MORPH_CLEANUP(morph_json_cleanup) = NULL;
	cJSON *files MORPH_CLEANUP(morph_json_cleanup) = NULL;
	char error[512];
	int rc;

	if (!input || !result || !tool_context)
		return -EINVAL;
	rc = patch_apply_authorized(tool_context_workdir_borrow(tool_context), input, &patch_result,
				    error, sizeof(error), authorize_patch_path, tool_context);
	if (rc != 0) {
		(void)tool_result_error_copy(result,
					     rc == -EACCES  ? "permission_denied"
					     : rc == -EPERM ? "approval_unavailable"
							    : "invalid_patch",
					     error[0] ? error : morph_strerror(rc));
		return rc;
	}
	data = cJSON_CreateObject();
	files = cJSON_CreateArray();
	if (!data || !files || !cJSON_AddBoolToObject(data, "changed", 1) ||
	    !cJSON_AddStringToObject(data, "workspace_root",
				     tool_context_workdir_borrow(tool_context)))
		return -ENOMEM;
	for (size_t i = 0; i < patch_result.changes.nelts; i++) {
		const struct patch_change *change = morph_array_borrow(&patch_result.changes, i);
		cJSON *item MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();

		if (!item || !cJSON_AddStringToObject(item, "path", change->path) ||
		    !cJSON_AddStringToObject(item, "action", patch_action_name(change->action)) ||
		    !cJSON_AddNumberToObject(item, "added", change->added) ||
		    !cJSON_AddNumberToObject(item, "removed", change->removed))
			return -ENOMEM;
		rc = morph_json_array_take(files, &item);
		if (rc != 0)
			return rc;
	}
	rc = morph_json_object_take(data, "files", &files);
	if (rc != 0)
		return rc;
	return tool_result_success_take(result, &data);
}

int apply_patch_init(struct tool_registry *registry,
		     struct tool_context *tool_context)
{
	morph_buf_t description MORPH_CLEANUP(morph_buf_cleanup) = {0};
	const char *workspace_root;
	struct tool_spec spec;
	int rc;
	static const char format[] =
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
		return -EINVAL;
	workspace_root = tool_context_workdir_borrow(tool_context);
	if (!workspace_root || !workspace_root[0])
		return -EINVAL;
	rc = morph_buf_init(&description, 1024);
	if (rc != 0)
		return rc;
	rc = morph_buf_printf(&description,
		"Apply a stripped-down Codex patch to text files. The workspace "
		"root is '%s'. Every patch path is relative to this exact directory; "
		"do not use absolute paths and do not repeat the workspace directory "
		"in a path. Use this tool for source editing instead of passing "
		"source content through shell commands. "
		"Use the exact envelope '*** Begin Patch\\n*** Add File: "
		"path\\n+content\\n*** End Patch'. Prefer bare '@@' for Update "
		"File hunks. An optional anchor after '@@ ' must be one complete "
		"source line copied verbatim, never a descriptive label. Numeric "
		"unified-diff ranges are not part of the Codex patch grammar. Hunk lines "
		"must start with space, +, or -. Move, Update, and Delete File are "
		"supported. Keep each call below "
		"4 KiB and at most 80 changed lines. Split large files across "
		"multiple calls by replacing a unique continuation marker in each "
		"successive Update File patch. Remove the marker in the final call. "
		"Every call must be a complete Codex patch. The final *** End Patch "
		"line must never have a leading +, space, or -. "
		"Do not emit unified-diff headers such as --- or +++. "
		"If a patch is truncated or its context does not match, read the "
		"file again and retry with a smaller complete patch.\n"
		"Update File example:\n"
		"*** Begin Patch\n"
		"*** Update File: relative/path.c\n"
		"@@\n"
		" unchanged context\n"
		"-old line\n"
		"+new line\n"
		"*** End Patch\n"
		"Add File example:\n"
		"*** Begin Patch\n"
		"*** Add File: relative/path.c\n"
		"+first content line\n"
		"+/* MORPH_CONTINUE */\n"
		"*** End Patch\n", workspace_root);
	if (rc != 0) {
		morph_buf_cleanup(&description);
		return rc;
	}
	memset(&spec, 0, sizeof(spec));
	spec = (struct tool_spec){
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "apply_patch",
		.title = "Apply patch",
		.description = morph_buf_cstr_borrow(&description),
		.input_kind = TOOL_INPUT_TEXT,
		.input_format = format,
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.flags = TOOL_FLAG_INTERNAL_APPROVAL,
		.exec = apply_patch_exec,
		.user_data = tool_context,
	};
	rc = tool_register_borrow(registry, &spec);
	morph_buf_cleanup(&description);
	return rc;
}
