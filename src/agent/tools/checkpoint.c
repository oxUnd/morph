#include "checkpoint.h"

#include "agent/tool.h"
#include "agent/tool_runtime.h"
#include "util/cleanup.h"
#include "util/error.h"
#include "util/json.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static int checkpoint_error(struct tool_result *result, int rc, const char *code,
			    const char *message, cJSON **details)
{
	int result_rc = tool_result_error_copy(result, code, message);

	if (result_rc == 0 && details && *details) {
		cJSON *meta MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();

		if (!meta)
			return -ENOMEM;
		result_rc = morph_json_object_take(meta, "checkpoint", details);
		if (result_rc == 0)
			result_rc = tool_result_set_meta_take(result, &meta);
	}
	return result_rc != 0 ? result_rc : rc;
}

static int checkpoint_exec(const char *input, struct tool_result *result, void *user_data)
{
	cJSON *arguments MORPH_CLEANUP(morph_json_cleanup) = NULL;
	cJSON *response MORPH_CLEANUP(morph_json_cleanup) = NULL;
	const struct tool_runtime_context *runtime;
	const char *action;
	const cJSON *id;
	int64_t target = 0;
	int rc;

	(void)user_data;
	if (!input || !result)
		return -EINVAL;
	arguments = cJSON_ParseWithOpts(input, NULL, 1);
	action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "action"));
	if (!cJSON_IsObject(arguments) || !action ||
	    (strcmp(action, "list") && strcmp(action, "preview") && strcmp(action, "restore_code")))
		return checkpoint_error(result, -EINVAL, "invalid_arguments",
					"Use checkpoint action=list, preview, or restore_code.",
					NULL);
	for (const cJSON *field = arguments->child; field; field = field->next) {
		if (!field->string ||
		    (strcmp(field->string, "action") && strcmp(field->string, "id")))
			return checkpoint_error(
				result, -EINVAL, "invalid_arguments",
				"Checkpoint accepts only action and an optional explicit id.",
				NULL);
	}
	id = cJSON_GetObjectItemCaseSensitive(arguments, "id");
	if (id) {
		if (!cJSON_IsNumber(id) || !isfinite(id->valuedouble) || id->valuedouble < 1 ||
		    id->valuedouble >= 9223372036854775808.0 ||
		    floor(id->valuedouble) != id->valuedouble)
			return checkpoint_error(
				result, -EINVAL, "invalid_arguments",
				"Checkpoint id must be an explicit positive integer.", NULL);
		target = (int64_t)id->valuedouble;
	}
	if (strcmp(action, "list") && !target)
		return checkpoint_error(
			result, -EINVAL, "checkpoint_id_required",
			"List checkpoints and preview an explicit id before restoring code.", NULL);
	runtime = result->runtime;
	if (runtime && runtime->agent_depth > 0)
		return checkpoint_error(
			result, -EPERM, "checkpoint_subagent_unsupported",
			"Code checkpoints are supported only in the main agent, not sub-agents.",
			NULL);
	if (!runtime || !runtime->checkpoint.request)
		return checkpoint_error(
			result, MORPH_ERR_NOT_CONFIGURED, "checkpoint_unavailable",
			"Code checkpoints are unavailable in this execution context.", NULL);
	rc = runtime->checkpoint.request(runtime->checkpoint.user_data, action, target, &response);
	if (rc != 0)
		return checkpoint_error(result, rc, "checkpoint_failed", morph_strerror(rc),
					&response);
	if (!cJSON_IsObject(response))
		return checkpoint_error(result, MORPH_ERR_PROTOCOL, "checkpoint_invalid_response",
					"Checkpoint returned no structured result. Do not assume "
					"code was restored.",
					&response);
	if (!strcmp(action, "list") && !cJSON_AddNumberToObject(response, "current_checkpoint_id",
								(double)runtime->checkpoint.id))
		return -ENOMEM;
	return tool_result_success_take(result, &response);
}

int checkpoint_init(struct tool_registry *registry)
{
	if (!registry)
		return -EINVAL;
	return tool_register_borrow(
		registry,
		&(struct tool_spec){
			.origin = TOOL_ORIGIN_BUILTIN,
			.name = "checkpoint",
			.title = "Code checkpoints",
			.description =
				"Inspect and undo tracked apply_patch edits. First list "
				"checkpoints. Each checkpoint precedes its turn; the current "
				"turn may still have zero files. To undo prior work, select "
				"the turn that made those edits. Read a "
				"preview to select the correct turn; preview and "
				"restore_code require its "
				"explicit id. restore_code restores tracked files only and "
				"reports conflicts "
				"with later manual edits. It does not rewind this "
				"conversation. Shell, "
				"manual, other-tool, and sub-agent changes are not "
				"captured. Use this tool "
				"for code undo; Git restore/reset/checkout require an "
				"explicit user request "
				"for a Git operation, never a guessed undo scope.",
			.input_schema = "{\"type\":\"object\",\"properties\":{"
					"\"action\":{\"type\":\"string\",\"enum\":[\"list\","
					"\"preview\",\"restore_code\"]},"
					"\"id\":{\"type\":\"integer\",\"minimum\":1}},"
					"\"required\":[\"action\"],\"additionalProperties\":false}",
			.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
			.flags = TOOL_FLAG_INTERNAL_APPROVAL,
			.exec = checkpoint_exec,
		});
}
