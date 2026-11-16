#include "guardrail_evidence.h"

#include "react.h"
#include "models/llm.h"
#include "media/image.h"
#include <nb/utf8.h>
#include <string.h>

#define GUARDRAIL_TRAJECTORY_MAX 32
#define GUARDRAIL_EXCERPT_MAX 2048

const struct react_step *guardrail_observation_borrow(const struct react_step *action,
						      const struct react_step **legacy_cursor)
{
	const struct react_step *observation = *legacy_cursor;

	if (action->tool_call_id) {
		for (const struct react_step *step = action->next; step; step = step->next) {
			if (step->type == REACT_STEP_OBSERVATION && step->tool_call_id &&
			    !strcmp(step->tool_call_id, action->tool_call_id))
				return step;
		}
	}
	while (observation && observation->type != REACT_STEP_OBSERVATION)
		observation = observation->next;
	/* Never pair an identified result with a different or unresolved call. */
	if (observation && observation->tool_call_id)
		return NULL;
	*legacy_cursor = observation ? observation->next : NULL;
	return observation;
}

int guardrail_local_path_copy(const char *path, const char *workdir, nb_buf *out)
{
	if (!path || !*path)
		return NB_ERR_INVAL;
	if (strstr(path, "://") && strncmp(path, "file://", 7))
		return GUARDRAIL_ARTIFACT_REMOTE;
	if (!strncmp(path, "file://", 7))
		path += 7;
	if (*path == '~')
		return nb_path_expand(out, path, NULL);
	if (workdir && *workdir && !nb_path_is_absolute(path))
		return nb_path_join(out, workdir, path);
	return nb_buf_append_cstr(out, path) ? NB_OK : NB_ERR_NOMEM;
}

int guardrail_artifact_inspect(const struct tool_artifact *artifact, const char *workdir,
			       nb_file_info *info)
{
	nb_buf path NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = guardrail_local_path_copy(artifact->path, workdir, &path);

	memset(info, 0, sizeof(*info));
	if (rc != 0)
		return rc;
	rc = nb_file_stat(nb_buf_cstr(&path), true, info, NULL);
	if (rc != NB_OK)
		return rc;
	if (!info->is_regular)
		return NB_ERR_FORMAT;
	if (artifact->kind != TOOL_ARTIFACT_FILE && info->size == 0)
		return NB_ERR_FORMAT;
	/* Image validation probes bounded headers, not a full pixel decode. */
	if (artifact->kind == TOOL_ARTIFACT_IMAGE) {
		const char *extension = nb_path_extension(nb_buf_cstr(&path));
		if (!strcmp(artifact->mime, "image/svg+xml") ||
		    !strcmp(artifact->mime, "image/webp") ||
		    !strcmp(artifact->mime, "image/avif") ||
		    !strcmp(artifact->mime, "image/heic") ||
		    (extension &&
		     (nb_str_eq_icase(nb_str_from_cstr(extension), NB_STR_LIT("svg")) ||
		      nb_str_eq_icase(nb_str_from_cstr(extension), NB_STR_LIT("webp")) ||
		      nb_str_eq_icase(nb_str_from_cstr(extension), NB_STR_LIT("avif")) ||
		      nb_str_eq_icase(nb_str_from_cstr(extension), NB_STR_LIT("heic")))))
			return GUARDRAIL_ARTIFACT_FORMAT_UNVERIFIED;
		return image_validate_file(nb_buf_cstr(&path));
	}
	return NB_OK;
}

static int evidence_text_add(cJSON *object, const char *key, const char *text, int *truncated)
{
	size_t length = text ? strlen(text) : 0;
	if (length <= GUARDRAIL_EXCERPT_MAX)
		return cJSON_AddStringToObject(object, key, text ? text : "") != NULL;
	nb_buf excerpt NB_CLEANUP(nb_buf_cleanup) = {0};
	size_t head = nb_utf8_clamp_bytes(text, GUARDRAIL_EXCERPT_MAX / 2);
	size_t tail = length - GUARDRAIL_EXCERPT_MAX / 2;
	while (tail < length && ((unsigned char)text[tail] & 0xc0) == 0x80)
		tail++;
	*truncated = 1;
	return nb_buf_append(&excerpt, text, head) &&
	       nb_buf_append_cstr(&excerpt, "\n[excerpt omitted]\n") &&
	       nb_buf_append_cstr(&excerpt, text + tail) &&
	       cJSON_AddStringToObject(object, key, nb_buf_cstr(&excerpt));
}

static cJSON *evidence_artifacts_create(const struct tool_artifact *items, int count,
					const char *workdir)
{
	cJSON *array NB_CLEANUP(cJSON_DeleteOwner) = tool_artifacts_json_create(items, count);
	if (!array)
		return NULL;
	for (int i = 0; i < count; i++) {
		nb_file_info info = {0};
		int rc = guardrail_artifact_inspect(&items[i], workdir, &info);
		cJSON *item = cJSON_GetArrayItem(array, i);
		const char *verification =
			rc == GUARDRAIL_ARTIFACT_FORMAT_UNVERIFIED ? "format_unverified"
			: rc > 0				   ? "remote_unverified"
			: rc < 0				   ? "invalid_or_unavailable"
			: items[i].kind == TOOL_ARTIFACT_IMAGE	   ? "image_header_valid"
								   : "regular_file";
		if (!cJSON_AddStringToObject(item, "verification", verification) ||
		    !cJSON_AddNumberToObject(item, "verification_error", rc < 0 ? rc : 0) ||
		    ((rc == 0 || rc == GUARDRAIL_ARTIFACT_FORMAT_UNVERIFIED) &&
		     !cJSON_AddNumberToObject(item, "actual_size_bytes", (double)info.size)))
			return NULL;
	}
	cJSON *owned = array;
	array = NULL;
	return owned;
}

static cJSON *evidence_call_create(const struct react_step *action,
				   const struct react_step *observation, const char *workdir,
				   int *truncated)
{
	cJSON *call NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!call || !cJSON_AddStringToObject(call, "tool", action->tool_name) ||
	    !cJSON_AddStringToObject(call, "call_id",
				     action->tool_call_id ? action->tool_call_id : "") ||
	    !evidence_text_add(call, "arguments", action->tool_args, truncated) ||
	    !cJSON_AddStringToObject(call, "outcome",
				     !observation		   ? "pending"
				     : observation->error_code < 0 ? "failed"
								   : "succeeded"))
		return NULL;
	if (observation) {
		cJSON *artifacts NB_CLEANUP(cJSON_DeleteOwner) = evidence_artifacts_create(
			observation->artifacts, observation->artifact_count, workdir);
		if (!artifacts ||
		    !cJSON_AddNumberToObject(call, "error_code", observation->error_code) ||
		    !cJSON_AddNumberToObject(call, "retry_policy",
					     guardrail_tool_retry_policy(observation)) ||
		    !evidence_text_add(call, "result", observation->content, truncated) ||
		    !cJSON_AddItemToObject(call, "artifacts", artifacts))
			return NULL;
		artifacts = NULL;
	}
	cJSON *owned = call;
	call = NULL;
	return owned;
}

cJSON *guardrail_evidence_create(const struct guardrail_eval_ctx *ctx)
{
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *calls NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateArray();
	cJSON *pending NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateArray();
	int total = 0;
	int succeeded = 0;
	int failed = 0;
	int index = 0;
	int truncated = 0;
	const struct react_step *cursor = ctx->steps;

	if (!root || !calls || !pending || !cJSON_AddNumberToObject(root, "version", 1) ||
	    !cJSON_AddStringToObject(root, "user_objective",
				     ctx->user_input ? ctx->user_input : "") ||
	    !cJSON_AddStringToObject(root, "candidate_answer",
				     ctx->proposed_answer ? ctx->proposed_answer : ""))
		return NULL;
	for (const struct react_step *step = ctx->steps; step; step = step->next)
		if (step->type == REACT_STEP_ACTION && step->tool_name)
			total++;
	for (const struct react_step *step = ctx->steps; step; step = step->next) {
		if (step->type != REACT_STEP_ACTION || !step->tool_name)
			continue;
		const struct react_step *observation = guardrail_observation_borrow(step, &cursor);
		if (observation) {
			succeeded += observation->error_code >= 0;
			failed += observation->error_code < 0;
		}
		if (index++ < total - GUARDRAIL_TRAJECTORY_MAX)
			continue;
		cJSON *call NB_CLEANUP(cJSON_DeleteOwner) =
			evidence_call_create(step, observation, ctx->workdir, &truncated);
		if (!call || !cJSON_AddItemToArray(calls, call))
			return NULL;
		call = NULL;
	}
	for (int i = 0; i < ctx->pending_call_count; i++) {
		const struct tool_call *call = &ctx->pending_calls[i];
		cJSON *item NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		if (!item || !cJSON_AddStringToObject(item, "tool", call->name) ||
		    !evidence_text_add(item, "arguments", call->arguments, &truncated) ||
		    !cJSON_AddItemToArray(pending, item))
			return NULL;
		item = NULL;
	}
	cJSON *current NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *artifacts NB_CLEANUP(cJSON_DeleteOwner) = evidence_artifacts_create(
		ctx->tool_artifacts ? ctx->tool_artifacts->items : NULL,
		ctx->tool_artifacts ? ctx->tool_artifacts->count : 0, ctx->workdir);
	if (!current || !artifacts ||
	    !cJSON_AddStringToObject(current, "tool", ctx->tool_name ? ctx->tool_name : "") ||
	    !evidence_text_add(current, "arguments", ctx->tool_args, &truncated) ||
	    !evidence_text_add(current, "result", ctx->tool_result, &truncated) ||
	    !cJSON_AddNumberToObject(current, "error_code", ctx->tool_error_code) ||
	    !cJSON_AddItemToObject(current, "artifacts", artifacts))
		return NULL;
	artifacts = NULL;
	if (!cJSON_AddItemToObject(root, "current_call", current))
		return NULL;
	current = NULL;
	if (!cJSON_AddItemToObject(root, "recent_calls", calls))
		return NULL;
	calls = NULL;
	if (!cJSON_AddItemToObject(root, "pending_calls", pending))
		return NULL;
	pending = NULL;
	int omitted = total > GUARDRAIL_TRAJECTORY_MAX ? total - GUARDRAIL_TRAJECTORY_MAX : 0;
	if (!cJSON_AddNumberToObject(root, "total_calls", total) ||
	    !cJSON_AddNumberToObject(root, "succeeded_calls", succeeded) ||
	    !cJSON_AddNumberToObject(root, "failed_calls", failed) ||
	    !cJSON_AddNumberToObject(root, "omitted_calls", omitted) ||
	    !cJSON_AddBoolToObject(root, "evidence_incomplete", truncated || omitted > 0))
		return NULL;
	cJSON *owned = root;
	root = NULL;
	return owned;
}
