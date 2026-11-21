#include "runtime/protocol_inspect.h"

#include "runtime/runtime.h"
#include "runtime/services.h"
#include "runtime/runtime_internal.h"
#include "agent/history.h"
#include "errors.h"
#include <nb/cleanup.h>

#include <string.h>

static void protocol_history_cleanup(struct model_history_item **owner)
{
	model_history_free_list(*owner);
	*owner = NULL;
}

static int protocol_context_kinds(struct runtime *runtime, cJSON *json)
{
	static const char *const names[] = {"user_message", "assistant_message",
		"assistant_tool_calls", "tool_result", "compaction_summary", "background_receipt"};
	int count = 0;
	struct model_history_item *items NB_CLEANUP(protocol_history_cleanup) =
		runtime_session_model_history_current_copy(runtime, 1, &count);
	if (count < 0)
		return MORPH_ERR_DB;

	cJSON *kinds = cJSON_AddArrayToObject(json, "kinds");
	if (!kinds)
		return NB_ERR_NOMEM;

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		int64_t tokens = 0;
		int64_t total = 0;
		for (const struct model_history_item *item = items; item; item = item->next) {
			if (!strcmp(item->kind, names[i])) {
				total++;
				tokens += item->token_count;
			}
		}

		cJSON *kind = cJSON_CreateObject();
		if (!kind)
			return NB_ERR_NOMEM;

		cJSON_AddItemToArray(kinds, kind);
		if (!cJSON_AddStringToObject(kind, "kind", names[i]) ||
		    !cJSON_AddNumberToObject(kind, "count", (double)total) ||
		    !cJSON_AddNumberToObject(kind, "tokens", (double)tokens))
			return NB_ERR_NOMEM;
	}

	return 0;
}

int runtime_protocol_context_get(struct runtime *runtime, cJSON **out)
{
	int active_items = 0, tokens = 0, tool_tokens = 0, compactions = 0, limit = 0;
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!json)
		return NB_ERR_NOMEM;

	int rc = runtime_session_model_context_stats(runtime, &active_items, &tokens,
		&tool_tokens, &compactions, &limit);
	if (rc != 0)
		return rc;

	int transcript_count = message_count(&runtime->context.database,
		runtime->context.current_session.id);
	if (transcript_count < 0)
		return MORPH_ERR_DB;

	if (!cJSON_AddNumberToObject(json, "active_items", active_items) ||
	    !cJSON_AddNumberToObject(json, "total_tokens", tokens) ||
	    !cJSON_AddNumberToObject(json, "tool_tokens", tool_tokens) ||
	    !cJSON_AddNumberToObject(json, "compactions", compactions) ||
	    !cJSON_AddNumberToObject(json, "limit", limit) ||
	    !cJSON_AddNumberToObject(json, "transcript_count", transcript_count))
		return NB_ERR_NOMEM;

	rc = protocol_context_kinds(runtime, json);
	if (rc != 0)
		return rc;

	struct runtime_history_compaction_status status = {0};
	if (runtime_session_compaction_status(runtime, &status) == 0) {
		cJSON *last = cJSON_AddObjectToObject(json, "last_compaction");
		if (!last || !cJSON_AddStringToObject(last, "trigger_kind", status.trigger_kind) ||
		    !cJSON_AddStringToObject(last, "status", status.status) ||
		    !cJSON_AddNumberToObject(last, "input_tokens", status.input_tokens) ||
		    !cJSON_AddNumberToObject(last, "output_tokens", status.output_tokens) ||
		    !cJSON_AddNumberToObject(last, "error_code", status.error_code))
			return NB_ERR_NOMEM;
	}

	*out = cJSON_StealOwner(&json);
	return 0;
}

int runtime_protocol_history_manage(struct runtime *runtime, const char *method, cJSON **out)
{
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!json)
		return NB_ERR_NOMEM;

	if (!strcmp(method, "history.compress")) {
		int removed = 0, kept = 0;
		int rc = runtime_session_compress(runtime, NULL, &removed, &kept);
		if (rc != 0)
			return rc;

		if (!cJSON_AddNumberToObject(json, "removed", removed) ||
		    !cJSON_AddNumberToObject(json, "window_removed", removed) ||
		    !cJSON_AddNumberToObject(json, "kept", kept))
			return NB_ERR_NOMEM;
	} else {
		struct agent_history_diagnostic diagnostic = {0};
		int changed = 0;
		int rc;
		if (!strcmp(method, "history.repair"))
			rc = runtime_session_history_repair(runtime, &diagnostic, &changed);
		else if (!strcmp(method, "history.diagnose"))
			rc = runtime_session_history_diagnose(runtime, &diagnostic);
		else
			return NB_ERR_UNSUPPORTED;

		if (rc != 0)
			return rc;

		cJSON *report = cJSON_AddObjectToObject(json, "report");
		if (!report ||
		    !cJSON_AddNumberToObject(report, "active_items", diagnostic.active_items) ||
		    !cJSON_AddNumberToObject(report, "dangling_calls", diagnostic.dangling_calls) ||
		    !cJSON_AddNumberToObject(report, "orphan_results", diagnostic.orphan_results) ||
		    !cJSON_AddNumberToObject(report, "invalid_payloads",
			diagnostic.invalid_payloads) ||
		    !cJSON_AddNumberToObject(report, "token_mismatches",
			diagnostic.token_mismatches) ||
		    !cJSON_AddBoolToObject(json, "changed", changed))
			return NB_ERR_NOMEM;
	}

	*out = cJSON_StealOwner(&json);
	return 0;
}

int runtime_protocol_turn_inspect(struct runtime *runtime, cJSON **out)
{
	struct runtime_turn_status status NB_CLEANUP(runtime_turn_status_cleanup) = {0};
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!runtime || !out)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	int rc = runtime_turn_status_copy(runtime, &status);

	if (rc != 0)
		return rc;

	json = cJSON_CreateObject();
	cJSON *steps = json ? cJSON_AddArrayToObject(json, "steps") : NULL;
	const char *state = status.state == REACT_STATE_DONE ? "done" :
		status.state == REACT_STATE_ABORT ? "abort" :
		status.state == REACT_STATE_TOOL_FAIL ? "tool_fail" : "unknown";

	if (!steps || !cJSON_AddStringToObject(json, "state", state) ||
	    !cJSON_AddStringToObject(json, "final_answer",
		status.final_answer ? status.final_answer : "") ||
	    !cJSON_AddStringToObject(json, "outcome", react_outcome_name(status.outcome)))
		return NB_ERR_NOMEM;

	if (status.last_error_code < 0 &&
	    (!cJSON_AddNumberToObject(json, "error_code", status.last_error_code) ||
	     !cJSON_AddStringToObject(json, "error", nb_err_str(morph_error_descriptions(),
		MORPH_ERROR_COUNT, status.last_error_code))))
		return NB_ERR_NOMEM;

	if (status.outcome_reason && status.outcome_reason[0] &&
	    !cJSON_AddStringToObject(json, "reason", status.outcome_reason))
		return NB_ERR_NOMEM;

	for (int i = 0; i < status.step_count; i++) {
		const struct runtime_trace_step *source = &status.steps[i];
		cJSON *step NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();

		if (!step ||
		    !cJSON_AddStringToObject(step, "type", react_step_type_name(source->type)) ||
		    (source->content &&
		     !cJSON_AddStringToObject(step, "content", source->content)) ||
		    (source->tool_name &&
		     !cJSON_AddStringToObject(step, "tool_name", source->tool_name)) ||
		    (source->tool_args &&
		     !cJSON_AddStringToObject(step, "tool_args", source->tool_args)) ||
		    !cJSON_AddItemToArray(steps, step))
			return NB_ERR_NOMEM;

		step = NULL;
	}

	*out = json;
	json = NULL;
	return 0;
}
