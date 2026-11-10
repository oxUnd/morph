#include "apply_patch.h"
#include "util/json.h"
#include "agent/agent_control.h"
#include "util/cleanup.h"
#include "agent/patch.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "agent/tool_runtime.h"
#include "cJSON.h"
#include "util/buf.h"
#include "util/error.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct patch_application {
	const struct tool_runtime_context *runtime;
	struct tool_context *context;
	const struct tool_checkpoint_context *checkpoint;
	int tracking;
	int begun;
	int captured;
	int capture_error;
	int authorization_error;
	morph_buf_t authorization_detail;
};

static void patch_application_cleanup(struct patch_application *application)
{
	morph_buf_cleanup(&application->authorization_detail);
}

static int authorize_patch_path(const char *path, int deletion, void *opaque)
{
	struct patch_application *application = opaque;
	struct tool_context *context = application->context;
	struct permission_review review = {0};
	struct tool_operation operation = {
		.review_out = &review,
		.kind = deletion ? TOOL_OP_PATH_DELETE : TOOL_OP_PATH_WRITE,
		.tool_name = "apply_patch",
		.principal = "tool:*",
		.target = path,
		.scope = tool_context_workdir_borrow(context),
	};
	int rc = application->runtime ? agent_control_write_check(
		application->runtime->agents, application->runtime->agent_id, path) : 0;
	int ownership_error = rc;
	if (rc == 0)
		rc = tool_context_check_operation(context, &operation);

	if (rc != 0) {
		application->authorization_error = rc;
		if (morph_buf_init(&application->authorization_detail, 256) == 0) {
			const char *id = application->runtime ? application->runtime->agent_id : NULL;
			(void)morph_buf_printf(&application->authorization_detail,
				"%s %s '%s' for agent '%s': %s. %s",
				ownership_error ? "Write ownership denied" : "Permission policy denied",
				deletion ? "deletion of" : "write to", path,
				id && *id ? id : "/root", morph_strerror(rc),
				ownership_error == -EBUSY ?
				"Another active agent owns this path; wait or use a disjoint path." :
				ownership_error ?
				"The path must be inside this agent's assigned write_paths." :
				review.reason[0] ? review.reason :
				"Permission was not granted. Do not bypass this decision through shell.");
		}
		return rc;
	}
	if (!application->tracking)
		return rc;
	rc = application->checkpoint->capture(application->checkpoint->user_data,
					      application->checkpoint->id, path);
	if (rc > 0)
		rc = MORPH_ERR_PROTOCOL;
	if (rc != 0)
		application->capture_error = rc;
	else
		application->captured++;
	return rc;
}

static int validate_patch_postimage(const char *path, size_t bytes, const char *removed_path,
				    void *opaque)
{
	struct patch_application *application = opaque;
	const struct tool_checkpoint_context *checkpoint = application->checkpoint;
	int rc = checkpoint->validate_write(checkpoint->user_data, checkpoint->id, path,
		bytes, removed_path);

	return rc > 0 ? MORPH_ERR_PROTOCOL : rc;
}

static cJSON *patch_checkpoint_status(const struct patch_application *application,
				      const char *status, int may_have_changed)
{
	cJSON *owner MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
	cJSON *output;

	if (!owner || !cJSON_AddNumberToObject(owner, "id", (double)application->checkpoint->id) ||
	    !cJSON_AddStringToObject(owner, "status", status) ||
	    !cJSON_AddBoolToObject(owner, "tracked",
				   application->begun && !strcmp(status, "sealed")) ||
	    !cJSON_AddNumberToObject(owner, "captured_paths", application->captured) ||
	    !cJSON_AddBoolToObject(owner, "changes_may_have_applied", may_have_changed))
		return NULL;
	output = owner;
	owner = NULL;
	return output;
}

static int patch_failure(struct tool_result *result, const struct patch_application *application,
			 int rc, const char *code, const char *message, const char *status,
			 int may_have_changed)
{
	cJSON *meta MORPH_CLEANUP(morph_json_cleanup) = NULL;
	cJSON *checkpoint MORPH_CLEANUP(morph_json_cleanup) = NULL;
	int result_rc;

	if (application->tracking)
		result_rc = tool_result_error_format(
			result, code, "%s Checkpoint %lld status: %s. %s", message,
			(long long)application->checkpoint->id, status,
			may_have_changed ? "Files may already have changed; inspect current files "
					   "and the checkpoint "
					   "before retrying or restoring."
					 : "No patch file mutations were performed.");
	else
		result_rc = tool_result_error_copy(result, code, message);
	if (result_rc != 0)
		return result_rc;
	if (!application->tracking)
		return rc;
	meta = cJSON_CreateObject();
	checkpoint = patch_checkpoint_status(application, status, may_have_changed);
	if (!meta || !checkpoint)
		return -ENOMEM;
	result_rc = morph_json_object_take(meta, "checkpoint", &checkpoint);
	if (result_rc == 0)
		result_rc = tool_result_set_meta_take(result, &meta);
	return result_rc != 0 ? result_rc : rc;
}

static int patch_success(struct tool_result *result, const struct patch_application *application,
			 const struct patch_result *patch_result)
{
	cJSON *data MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
	cJSON *files MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateArray();
	int rc;

	if (!data || !files || !cJSON_AddBoolToObject(data, "changed", 1) ||
	    !cJSON_AddStringToObject(data, "workspace_root",
				     tool_context_workdir_borrow(application->context)))
		return -ENOMEM;
	for (size_t i = 0; i < patch_result->changes.nelts; i++) {
		const struct patch_change *change = morph_array_borrow(&patch_result->changes, i);
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
	if (rc == 0 && application->tracking) {
		cJSON *checkpoint MORPH_CLEANUP(morph_json_cleanup) =
			patch_checkpoint_status(application, "sealed", 1);

		if (!checkpoint)
			return -ENOMEM;
		rc = morph_json_object_take(data, "checkpoint", &checkpoint);
	}
	return rc != 0 ? rc : tool_result_success_take(result, &data);
}

struct patch_agent_guard {
	struct agent_control *control;
};

static void patch_agent_guard_cleanup(struct patch_agent_guard *guard)
{
	agent_control_patch_end(guard->control);
}

static int apply_patch_exec(const char *input, struct tool_result *result, void *user_data)
{
	struct patch_result patch_result MORPH_CLEANUP(patch_result_cleanup) = {0};
	struct patch_application application MORPH_CLEANUP(patch_application_cleanup) = {
		.context = user_data,
	};
	const struct tool_runtime_context *runtime;
	char error[512] = {0};
	int finish_rc = 0;
	int rc;

	if (!input || !result || !application.context)
		return -EINVAL;
	runtime = result->runtime;
	application.runtime = runtime;
	struct patch_agent_guard guard MORPH_CLEANUP(patch_agent_guard_cleanup) = {
		.control = runtime ? runtime->agents : NULL,
	};
	agent_control_patch_begin(guard.control);
	if (runtime && runtime->agent_depth == 0) {
		application.checkpoint = &runtime->checkpoint;
		application.tracking = runtime->checkpoint.begin || runtime->checkpoint.capture ||
				       runtime->checkpoint.finish;
	}
	if (application.tracking) {
		const struct tool_checkpoint_context *checkpoint = application.checkpoint;

		if (!checkpoint->begin || !checkpoint->capture || !checkpoint->finish ||
		    checkpoint->id <= 0)
			return patch_failure(result, &application, MORPH_ERR_NOT_CONFIGURED,
					     "checkpoint_tracking_unavailable",
					     "Checkpoint tracking is incomplete.", "unavailable",
					     0);
		rc = checkpoint->begin(checkpoint->user_data, checkpoint->id);
		if (rc != 0)
			return patch_failure(result, &application, rc < 0 ? rc : MORPH_ERR_PROTOCOL,
					     "checkpoint_begin_failed",
					     "Could not begin checkpoint tracking.", "begin_failed",
					     0);
		application.begun = 1;
	}
	struct patch_write_policy policy = {
		.max_file_bytes = application.tracking ? application.checkpoint->max_file_bytes : 0,
		.validate = application.tracking && application.checkpoint->validate_write
			? validate_patch_postimage : NULL,
		.user_data = &application,
	};
	rc = patch_apply_authorized_with_policy(
		tool_context_workdir_borrow(application.context), input, &patch_result,
		error, sizeof(error), authorize_patch_path, &application, &policy);
	if (application.begun) {
		finish_rc = application.checkpoint->finish(application.checkpoint->user_data,
							   application.checkpoint->id);
		if (finish_rc > 0)
			finish_rc = MORPH_ERR_PROTOCOL;
	}
	if (finish_rc != 0)
		return patch_failure(result, &application, finish_rc, "checkpoint_seal_failed",
				     "Could not seal checkpoint tracking after the patch attempt.",
				     "seal_failed",
				     application.captured > 0 && !application.capture_error &&
					     !application.authorization_error);
	if (rc != 0) {
		const char *code = application.capture_error ? "checkpoint_capture_failed"
				   : rc == -EACCES	     ? "permission_denied"
				   : rc == -EPERM	     ? "approval_unavailable"
							     : "invalid_patch";
		const char *status = application.capture_error ? "capture_failed" : "sealed";

		return patch_failure(result, &application, rc, code,
				     application.capture_error
					     ? "Could not capture file preimages before editing."
				     : application.authorization_detail.len ?
					     morph_buf_cstr_borrow(&application.authorization_detail)
				     : error[0] ? error : morph_strerror(rc),
				     status,
				     application.captured > 0 && !application.capture_error &&
					     !application.authorization_error);
	}
	rc = patch_success(result, &application, &patch_result);
	if (rc != 0)
		return patch_failure(
			result, &application, rc, "patch_receipt_failed",
			"Patch files were changed, but the tool receipt could not be created.",
			"sealed", 1);
	return 0;
}

int apply_patch_init(struct tool_registry *registry, struct tool_context *tool_context)
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
	rc = morph_buf_printf(
		&description,
		"Apply a stripped-down Codex patch to text files. The workspace "
		"root is '%s'. Every patch path is relative to this exact directory; "
		"do not use absolute paths and do not repeat the workspace directory "
		"in a path. Use this tool for source editing instead of passing "
		"source content through shell commands. Main-agent runtime checkpoints capture "
		"these edits; to undo code, use checkpoint list and preview to select the "
		"correct turn, then restore_code with its explicit id. Shell, manual, other-tool, "
		"and sub-agent edits are not captured. Failed patches may have partially "
		"changed files; inspect the checkpoint and current files before retrying. "
		"Use the exact envelope '*** Begin Patch\\n*** Add File: "
		"path\\n+content\\n*** End Patch'. Prefer bare '@@' for Update "
		"File hunks. An optional anchor after '@@ ' must be one complete "
		"source line copied verbatim, never a descriptive label. Numeric "
		"unified-diff ranges are not part of the Codex patch grammar. Hunk lines "
		"must start with space, +, or -. Move, Update, and Delete File are "
			"supported. Each complete patch may contain up to 64 KiB. "
			"Group related edits in one call when they fit. Split larger edits "
			"into complete patches at natural file or hunk boundaries. "
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
		"*** End Patch\n",
		workspace_root);
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
