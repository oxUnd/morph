#include "agent/turn.h"

#include "agent/tokenizer.h"
#include "session.h"
#include "util/arena.h"
#include "util/error.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static unsigned turn_flags(const struct agent_session_runtime *runtime)
{
	if (!runtime || runtime->flags == 0)
		return AGENT_TURN_DEFAULT_FLAGS;
	return runtime->flags;
}

static const char *turn_model_input(const struct agent_turn *turn)
{
	return turn->input.model_input ? turn->input.model_input : "";
}

static const char *turn_stored_user_input(const struct agent_turn *turn)
{
	if (turn->input.stored_user_input)
		return turn->input.stored_user_input;
	return turn_model_input(turn);
}

static const char *turn_id_value(const struct agent_turn *turn)
{
	if (turn->input.turn_id && turn->input.turn_id[0])
		return turn->input.turn_id;
	return react_get_turn_id(turn->runtime.react);
}

static void turn_set_first_error(int *rc, int err)
{
	if (err < 0 && *rc == 0)
		MORPH_SET_ERR(*rc, err);
}

static int turn_emit_background(const struct agent_session_runtime *runtime,
				const char *name, const char *phase,
				const char *message, int error_code)
{
	if (!runtime->background_cb)
		return 0;
	return runtime->background_cb(runtime->background_user_data, name,
				      phase, message, "memory_consolidation",
				      -1, error_code);
}

static int turn_validate_runtime(const struct agent_session_runtime *runtime)
{
	if (!runtime || !runtime->db || !runtime->db->handle ||
	    runtime->session_id <= 0 || !runtime->react)
		MORPH_RETURN(-EINVAL);
	return 0;
}

int agent_session_load_history(const struct agent_session_runtime *runtime)
{
	struct message *list;
	struct message *cur;
	int count = 0;
	int rc;

	rc = turn_validate_runtime(runtime);
	if (rc != 0)
		return rc;

	msg_list_destroy(runtime->react->messages);
	runtime->react->messages = NULL;
	if (runtime->react->session_arena)
		arena_reset(runtime->react->session_arena);

	list = message_list(runtime->db, runtime->session_id, &count);
	cur = list;
	while (cur) {
		struct message_list *m;

		m = msg_list_create(runtime->react->session_arena, cur->role,
				    cur->content, cur->token_count);
		if (!m) {
			message_free_list(list);
			MORPH_RETURN(-ENOMEM);
		}
		m->compressed = cur->compressed;
		msg_list_append(&runtime->react->messages, m);
		cur = cur->next;
	}
	message_free_list(list);
	return 0;
}

static int turn_build_memory_context(struct agent_turn *turn)
{
	char *memory_ctx;
	int rc;

	if (!(turn_flags(&turn->runtime) & AGENT_TURN_BUILD_MEMORY_CONTEXT))
		return 0;

	memory_ctx = memory_build_context(
		turn->runtime.db, turn->runtime.session_id,
		turn_model_input(turn), turn->runtime.memory_options);
	rc = react_set_memory_context(turn->runtime.react, memory_ctx);
	free(memory_ctx);
	return rc;
}

int agent_turn_begin(struct agent_turn *turn,
		     const struct agent_session_runtime *runtime,
		     const struct agent_turn_input *input)
{
	unsigned flags;
	int rc;

	if (!turn || !input || !input->model_input)
		MORPH_RETURN(-EINVAL);
	memset(turn, 0, sizeof(*turn));

	rc = turn_validate_runtime(runtime);
	if (rc != 0)
		return rc;

	turn->runtime = *runtime;
	turn->input = *input;
	flags = turn_flags(&turn->runtime);

	if (flags & AGENT_TURN_LOAD_HISTORY) {
		rc = agent_session_load_history(&turn->runtime);
		if (rc != 0)
			return rc;
	}

	if (turn->input.turn_id && turn->input.turn_id[0]) {
		rc = react_set_turn_id(turn->runtime.react, turn->input.turn_id);
		if (rc != 0)
			return rc;
	}

	rc = turn_build_memory_context(turn);
	if (rc != 0)
		return rc;

	turn->begun = 1;
	return 0;
}

static int turn_save_trace(struct agent_turn *turn)
{
	cJSON *arr;
	char *json;
	struct react_step *cur;
	int round_no;
	int aborted;
	int rc;

	if (!turn->runtime.react || !turn->runtime.react->steps)
		return 0;

	arr = cJSON_CreateArray();
	if (!arr)
		MORPH_RETURN(-ENOMEM);

	cur = turn->runtime.react->steps;
	while (cur) {
		cJSON *obj = cJSON_CreateObject();
		if (!obj) {
			cJSON_Delete(arr);
			MORPH_RETURN(-ENOMEM);
		}
		if (!cJSON_AddStringToObject(obj, "type",
					     react_step_type_name(cur->type))) {
			cJSON_Delete(obj);
			cJSON_Delete(arr);
			MORPH_RETURN(-ENOMEM);
		}
		if (cur->content &&
		    !cJSON_AddStringToObject(obj, "content", cur->content)) {
			cJSON_Delete(obj);
			cJSON_Delete(arr);
			MORPH_RETURN(-ENOMEM);
		}
		if (cur->tool_name &&
		    !cJSON_AddStringToObject(obj, "tool_name",
					     cur->tool_name)) {
			cJSON_Delete(obj);
			cJSON_Delete(arr);
			MORPH_RETURN(-ENOMEM);
		}
		if (cur->tool_args &&
		    !cJSON_AddStringToObject(obj, "tool_args",
					     cur->tool_args)) {
			cJSON_Delete(obj);
			cJSON_Delete(arr);
			MORPH_RETURN(-ENOMEM);
		}
		if (cur->tool_call_id &&
		    !cJSON_AddStringToObject(obj, "tool_call_id",
					     cur->tool_call_id)) {
			cJSON_Delete(obj);
			cJSON_Delete(arr);
			MORPH_RETURN(-ENOMEM);
		}
		if (cur->error_code < 0)
			cJSON_AddNumberToObject(obj, "error_code",
						cur->error_code);
		if (cur->artifacts.count > 0) {
			cJSON *artifacts =
				tool_artifact_list_to_json(&cur->artifacts);
			if (artifacts)
				cJSON_AddItemToObject(obj, "artifacts",
						      artifacts);
		}
		cJSON_AddItemToArray(arr, obj);
		cur = cur->next;
	}

	json = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);
	if (!json)
		MORPH_RETURN(-ENOMEM);

	round_no = trace_get_next_round_no(turn->runtime.db,
					   turn->runtime.session_id);
	aborted = turn->runtime.react->state == REACT_STATE_ABORT ? 1 : 0;
	rc = trace_save(turn->runtime.db, turn->runtime.session_id, round_no,
			json, aborted);
	free(json);
	return rc;
}

static int turn_persist_message(struct agent_turn *turn,
				const char *role, const char *content,
				int token_count)
{
	const char *tid = turn_id_value(turn);

	if (tid && tid[0])
		return message_add_with_turn_id(
			turn->runtime.db, turn->runtime.session_id, role,
			content, token_count, tid);
	return message_add(turn->runtime.db, turn->runtime.session_id, role,
			   content, token_count);
}

static int turn_persist_user(struct agent_turn *turn,
			     struct agent_turn_result *result)
{
	const char *input = turn_stored_user_input(turn);
	int rc;

	if (!(turn_flags(&turn->runtime) & AGENT_TURN_SAVE_EMPTY_USER) &&
	    input[0] == '\0')
		return 0;

	result->user_tokens =
		tokenizer_count(turn->runtime.react->tokenizer, input);
	rc = turn_persist_message(turn, "user", input, result->user_tokens);
	if (rc < 0)
		return rc;
	result->user_saved = 1;

	if (!(turn_flags(&turn->runtime) & AGENT_TURN_UPDATE_TOKENS))
		return 0;
	return session_update_tokens(turn->runtime.db, turn->runtime.session_id,
				     result->user_tokens);
}

static int turn_persist_assistant(struct agent_turn *turn,
				  struct agent_turn_result *result,
				  char **assistant_for_memory)
{
	const char *answer;
	char *rendered = NULL;
	int rc;

	*assistant_for_memory = NULL;
	answer = turn->runtime.react->final_answer;
	if (!answer)
		return 0;
	if (!(turn_flags(&turn->runtime) & AGENT_TURN_SAVE_EMPTY_ASSISTANT) &&
	    answer[0] == '\0')
		return 0;

	if (turn->runtime.render_assistant) {
		rendered = turn->runtime.render_assistant(
			answer, turn->runtime.render_user_data);
		if (rendered)
			answer = rendered;
	}

	result->assistant_tokens =
		tokenizer_count(turn->runtime.react->tokenizer, answer);
	rc = turn_persist_message(turn, "assistant", answer,
				  result->assistant_tokens);
	if (rc < 0) {
		free(rendered);
		return rc;
	}
	result->assistant_saved = 1;

	if (turn_flags(&turn->runtime) & AGENT_TURN_UPDATE_TOKENS) {
		rc = session_update_tokens(turn->runtime.db,
					   turn->runtime.session_id,
					   result->assistant_tokens);
		if (rc < 0) {
			free(rendered);
			return rc;
		}
	}

	*assistant_for_memory = rendered ? rendered : strdup(answer);
	if (!*assistant_for_memory)
		MORPH_RETURN(-ENOMEM);
	return 0;
}

static int turn_consolidate_memory(struct agent_turn *turn,
				   struct agent_turn_result *result,
				   const char *assistant_output)
{
	int rc;
	int success;

	if (!(turn_flags(&turn->runtime) & AGENT_TURN_CONSOLIDATE_MEMORY))
		return 0;
	success = turn->runtime.react &&
		turn->runtime.react->state == REACT_STATE_DONE &&
		(turn->runtime.react->outcome == REACT_OUTCOME_SUCCESS ||
		 turn->runtime.react->outcome == REACT_OUTCOME_NONE);
	if (!success)
		return 0;

	if (turn_flags(&turn->runtime) & AGENT_TURN_ASYNC_MEMORY) {
		turn_emit_background(&turn->runtime, "background.started",
				     "begin",
				     "memory consolidation queued", 0);
		rc = memory_consolidate_turn_async(
			turn->runtime.db, turn->runtime.session_id,
			turn_model_input(turn), assistant_output,
			turn->runtime.react->steps,
			turn->runtime.react->state == REACT_STATE_DONE,
			turn->runtime.memory_options);
		if (rc == 0) {
			result->memory_queued = 1;
			turn_emit_background(&turn->runtime, "background.ready",
					     "ready",
					     "memory consolidation queued", 0);
			return 0;
		}

		turn_emit_background(&turn->runtime, "background.progress",
				     "progress",
				     "memory consolidation running inline", rc);
	}

	rc = memory_consolidate_turn(
		turn->runtime.db, turn->runtime.session_id,
		turn_model_input(turn), assistant_output,
		turn->runtime.react->steps,
		turn->runtime.react->state == REACT_STATE_DONE,
		turn->runtime.memory_options);
	result->memory_ran_inline = 1;
	result->memory_rc = rc;

	if (turn_flags(&turn->runtime) & AGENT_TURN_ASYNC_MEMORY) {
		turn_emit_background(&turn->runtime,
				     rc == 0 ? "background.completed" :
					       "background.failed",
				     rc == 0 ? "end" : "failed",
				     rc == 0 ?
					     "memory consolidation completed" :
					     "memory consolidation failed",
				     rc);
	}
	return rc;
}

int agent_turn_finish(struct agent_turn *turn, struct agent_turn_result *result)
{
	struct agent_turn_result local_result;
	char *assistant_for_memory = NULL;
	const char *memory_answer;
	unsigned flags;
	int rc = 0;
	int step_rc;

	if (!turn || !turn->begun)
		MORPH_RETURN(-EINVAL);
	if (turn->finished)
		MORPH_RETURN(-EALREADY);
	step_rc = turn_validate_runtime(&turn->runtime);
	if (step_rc != 0)
		return step_rc;

	memset(&local_result, 0, sizeof(local_result));
	flags = turn_flags(&turn->runtime);
	local_result.history_loaded =
		(flags & AGENT_TURN_LOAD_HISTORY) ? 1 : 0;
	local_result.memory_context_built =
		(flags & AGENT_TURN_BUILD_MEMORY_CONTEXT) ? 1 : 0;

	if (flags & AGENT_TURN_SAVE_TRACE) {
		step_rc = turn_save_trace(turn);
		if (step_rc == 0)
			local_result.trace_saved =
				turn->runtime.react->steps ? 1 : 0;
		turn_set_first_error(&rc, step_rc);
	}

	if (flags & AGENT_TURN_SAVE_MESSAGES) {
		step_rc = turn_persist_user(turn, &local_result);
		turn_set_first_error(&rc, step_rc);

		step_rc = turn_persist_assistant(
			turn, &local_result, &assistant_for_memory);
		turn_set_first_error(&rc, step_rc);
	}

	memory_answer = assistant_for_memory ?
		assistant_for_memory : turn->runtime.react->final_answer;
	step_rc = turn_consolidate_memory(turn, &local_result, memory_answer);
	turn_set_first_error(&rc, step_rc);

	free(assistant_for_memory);
	turn->finished = 1;
	if (result)
		*result = local_result;
	return rc;
}
