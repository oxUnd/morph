#include "agent/turn.h"

#include "agent/history.h"
#include "agent/tokenizer.h"
#include "session.h"
#include "util/cleanup.h"
#include "util/error.h"
#include "util/id.h"
#include "util/buf.h"
#include "util/utf8.h"
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
	return react_turn_id_borrow(turn->runtime.react);
}

static void turn_set_first_error(int *rc, int err)
{
	if (err < 0 && *rc == 0)
		*rc = (err);
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
		return -EINVAL;
	return 0;
}

static void turn_unbind_history(struct react_context *react)
{
	if (!react)
		return;
	react->history_db = NULL;
	react->history_session_id = 0;
	react->history_enabled = 0;
	react->memory_options = NULL;
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
	model_history_free_list(runtime->react->history_items);
	runtime->react->history_items = NULL;

	rc = model_history_migrate_messages(runtime->db, runtime->session_id);
	if (rc != 0)
		return rc;
	rc = agent_history_repair(runtime->db, runtime->session_id,
		runtime->react->tokenizer, NULL, NULL);
	if (rc != 0)
		return rc;
	runtime->react->history_items = model_history_list(
		runtime->db, runtime->session_id, 1, &count);
	if (!runtime->react->history_items && count != 0)
		return MORPH_ERR_DB;

	count = 0;
	list = message_list(runtime->db, runtime->session_id, &count);
	cur = list;
	while (cur) {
		struct message_list *m MORPH_CLEANUP(msg_list_clear) = NULL;

		m = msg_list_create(cur->role, cur->content, cur->token_count);
		if (!m) {
			message_free_list(list);
			return -ENOMEM;
		}
		m->compressed = cur->compressed;
		(void)msg_list_append_take(&runtime->react->messages, &m);
		cur = cur->next;
	}
	message_free_list(list);
	return 0;
}

static int turn_prepare_memory_reply(struct agent_turn *turn)
{
	char *reply_memory MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	int rc;

	if (!(turn_flags(&turn->runtime) & AGENT_TURN_PREPARE_MEMORY_REPLY))
		return 0;

	rc = memory_reply_text_create(turn->runtime.db, turn->runtime.session_id,
				      turn_model_input(turn), turn->runtime.memory_options,
				      &reply_memory);
	if (rc != 0)
		return rc;
	rc = react_set_memory_reply_text_copy(turn->runtime.react, reply_memory);
	if (rc == 0 && reply_memory)
		turn->runtime.react->memory_reply_session_id = turn->runtime.session_id;
	return rc;
}

int agent_turn_begin(struct agent_turn *turn, const struct agent_session_runtime *runtime,
		     const struct agent_turn_input *input)
{
	unsigned flags;
	int rc;

	if (!turn || !input || !input->model_input)
		return -EINVAL;
	memset(turn, 0, sizeof(*turn));

	rc = turn_validate_runtime(runtime);
	if (rc != 0)
		return rc;

	turn->runtime = *runtime;
	turn->input = *input;
	flags = turn_flags(&turn->runtime);
	turn->runtime.react->history_db = turn->runtime.db;
	turn->runtime.react->history_session_id = turn->runtime.session_id;
	turn->runtime.react->history_enabled =
		(flags & AGENT_TURN_SAVE_MESSAGES) ? 1 : 0;
	turn->runtime.react->history_error = 0;

	if (flags & AGENT_TURN_LOAD_HISTORY) {
		rc = agent_session_load_history(&turn->runtime);
		if (rc != 0) {
			turn_unbind_history(turn->runtime.react);
			return rc;
		}
	}

	if (turn->input.turn_id && turn->input.turn_id[0]) {
		rc = react_set_turn_id(turn->runtime.react, turn->input.turn_id);
		if (rc != 0) {
			turn_unbind_history(turn->runtime.react);
			return rc;
		}
	} else {
		char turn_id[64];

		rc = morph_random_id("turn_", turn_id, sizeof(turn_id));
		if (rc != 0) {
			turn_unbind_history(turn->runtime.react);
			return rc;
		}
		rc = react_set_turn_id(turn->runtime.react, turn_id);
		if (rc != 0) {
			turn_unbind_history(turn->runtime.react);
			return rc;
		}
	}
	if (turn->runtime.react->history_enabled) {
		rc = agent_history_maybe_compact(turn->runtime.react);
		if (rc < 0) {
			turn_unbind_history(turn->runtime.react);
			return rc;
		}
	}

	if (flags & AGENT_TURN_SAVE_MESSAGES) {
		const char *stored = turn->input.stored_user_input ?
			turn->input.stored_user_input : turn->input.model_input;
		int tokens = tokenizer_count(turn->runtime.react->tokenizer,
			stored);

		rc = db_exec(turn->runtime.db, "BEGIN IMMEDIATE;");
		if (rc == 0)
			rc = message_add_with_turn_id(turn->runtime.db,
				turn->runtime.session_id, "user", stored, tokens,
				turn->runtime.react->turn_id);
		if (rc == 0)
			rc = agent_history_record_user(turn->runtime.react,
				turn->input.model_input);
		if (rc == 0)
			rc = db_exec(turn->runtime.db, "COMMIT;");
		if (rc != 0) {
			(void)db_exec(turn->runtime.db, "ROLLBACK;");
			turn_unbind_history(turn->runtime.react);
			return rc;
		}
	}

	rc = memory_accept_input(runtime->db, runtime->session_id, turn_stored_user_input(turn),
				 react_turn_id_borrow(runtime->react), runtime->memory_options);
	if (rc == 0)
		rc = turn_prepare_memory_reply(turn);
	if (rc != 0) {
		turn_unbind_history(turn->runtime.react);
		return rc;
	}
	runtime->react->memory_options = runtime->memory_options;

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
		return -ENOMEM;

	cur = turn->runtime.react->steps;
	while (cur) {
		cJSON *obj = cJSON_CreateObject();
		if (!obj) {
			cJSON_Delete(arr);
			return -ENOMEM;
		}
		if (!cJSON_AddStringToObject(obj, "type",
					     react_step_type_name(cur->type))) {
			cJSON_Delete(obj);
			cJSON_Delete(arr);
			return -ENOMEM;
		}
		if (cur->content &&
		    !cJSON_AddStringToObject(obj, "content", cur->content)) {
			cJSON_Delete(obj);
			cJSON_Delete(arr);
			return -ENOMEM;
		}
		if (cur->tool_name &&
		    !cJSON_AddStringToObject(obj, "tool_name",
					     cur->tool_name)) {
			cJSON_Delete(obj);
			cJSON_Delete(arr);
			return -ENOMEM;
		}
		if (cur->tool_args &&
		    !cJSON_AddStringToObject(obj, "tool_args",
					     cur->tool_args)) {
			cJSON_Delete(obj);
			cJSON_Delete(arr);
			return -ENOMEM;
		}
		if (cur->tool_call_id &&
		    !cJSON_AddStringToObject(obj, "tool_call_id",
					     cur->tool_call_id)) {
			cJSON_Delete(obj);
			cJSON_Delete(arr);
			return -ENOMEM;
		}
		if (cur->error_code < 0)
			cJSON_AddNumberToObject(obj, "error_code",
						cur->error_code);
		if (cur->artifact_count > 0) {
			cJSON *artifacts =
				tool_artifacts_json_create(cur->artifacts, cur->artifact_count);
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
		return -ENOMEM;

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
		return -ENOMEM;
	return 0;
}

static void turn_memory_statement_cleanup(sqlite3_stmt **statement)
{
	sqlite3_finalize(*statement);
}

static int turn_memory_input_create(struct agent_turn *turn, char **out)
{
	sqlite3_stmt *stmt MORPH_CLEANUP(turn_memory_statement_cleanup) = NULL;
	morph_buf_t input MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int step;

	if (sqlite3_prepare_v2(
		    turn->runtime.db->handle,
		    "SELECT content FROM (SELECT content,sequence_no FROM model_history_items "
		    "WHERE session_id=? AND turn_id=? AND role='user' AND kind='user_message' "
		    "AND COALESCE(json_extract(payload_json,'$.source'),'') != 'hook' "
		    "AND active=1 ORDER BY sequence_no DESC LIMIT 4) ORDER BY sequence_no",
		    -1, &stmt, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, turn->runtime.session_id);
	sqlite3_bind_text(stmt, 2, turn_id_value(turn), -1, SQLITE_TRANSIENT);
	int rc = morph_buf_init(&input, 2048);

	if (rc != 0)
		return rc;
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *text = (const char *)sqlite3_column_text(stmt, 0);

		if (!text)
			continue;
		size_t length = utf8_clamp_bytes(text, 1800);

		rc = morph_buf_printf(
			&input, "User input or correction:\n%.*s%s\n", (int)length, text,
			text[length] ? " [excerpt; full input remains in history]" : "");
		if (rc != 0)
			return rc;
	}
	if (step != SQLITE_DONE)
		return MORPH_ERR_DB;
	if (!input.len)
		rc = morph_buf_puts(&input, turn_stored_user_input(turn));
	if (rc == 0)
		*out = morph_buf_detach(&input);
	return rc;
}

static int turn_save_memory(struct agent_turn *turn, struct agent_turn_result *result,
			    const char *assistant_output)
{
	int rc;
	int success;
	struct memory_options background = {0};
	char *memory_input MORPH_CLEANUP(morph_charp_cleanup) = NULL;

	if (turn->runtime.memory_options)
		background = *turn->runtime.memory_options;
	background.capture_input_enabled = 0;

	if (!background.enabled || !(turn_flags(&turn->runtime) & AGENT_TURN_SAVE_MEMORY))
		return 0;
	success = turn->runtime.react &&
		turn->runtime.react->state == REACT_STATE_DONE &&
		(turn->runtime.react->outcome == REACT_OUTCOME_SUCCESS ||
		 turn->runtime.react->outcome == REACT_OUTCOME_NONE);
	if (!success || (!background.save_tasks_enabled && !background.learn_from_chats_enabled))
		return 0;
	rc = turn_memory_input_create(turn, &memory_input);
	if (rc != 0)
		return rc;

	if (turn_flags(&turn->runtime) & AGENT_TURN_ASYNC_MEMORY) {
		turn_emit_background(&turn->runtime, "background.started", "begin",
				     "memory update scheduling", 0);
		rc = memory_save_turn_async(
			turn->runtime.db, turn->runtime.session_id, memory_input, assistant_output,
			turn->runtime.react->steps, turn->runtime.react->state == REACT_STATE_DONE,
			&background);
		if (rc == 0) {
			result->memory_queued = 1;
			turn_emit_background(&turn->runtime, "background.ready", "ready",
					     "memory update queued", 0);
			return 0;
		}

		turn_emit_background(&turn->runtime, "background.progress", "progress",
				     "memory update running inline", rc);
	}

	rc = memory_save_turn(turn->runtime.db, turn->runtime.session_id, memory_input,
			      assistant_output, turn->runtime.react->steps,
			      turn->runtime.react->state == REACT_STATE_DONE, &background);
	result->memory_ran_inline = 1;
	result->memory_rc = rc;

	if (turn_flags(&turn->runtime) & AGENT_TURN_ASYNC_MEMORY) {
		turn_emit_background(
			&turn->runtime, rc == 0 ? "background.completed" : "background.failed",
			rc == 0 ? "end" : "failed",
			rc == 0 ? "memory update completed" : "memory update failed", rc);
	}
	return rc;
}

static int turn_model_history_succeeded(const struct agent_turn *turn)
{
	const struct react_context *react;

	if (!turn || !turn->runtime.react)
		return 0;
	react = turn->runtime.react;
	return react->state == REACT_STATE_DONE &&
		(react->outcome == REACT_OUTCOME_SUCCESS ||
		 react->outcome == REACT_OUTCOME_NONE);
}

static int turn_has_error_code(const struct agent_turn *turn, int error_code)
{
	const struct react_context *react;
	const struct react_step *step;

	if (!turn || !turn->runtime.react)
		return 0;
	react = turn->runtime.react;
	if (react->last_error_code == error_code)
		return 1;
	for (step = react->steps; step; step = step->next) {
		if (step->error_code == error_code)
			return 1;
	}
	return 0;
}

static int turn_should_keep_user_history(const struct agent_turn *turn)
{
	const struct react_context *react;

	if (!turn || !turn->runtime.react)
		return 0;
	react = turn->runtime.react;
	return react->outcome == REACT_OUTCOME_LLM_ERROR ||
		react->outcome == REACT_OUTCOME_MAX_ITERATIONS ||
		react->terminal_tool_error < 0;
}

static int turn_should_keep_complete_history(const struct agent_turn *turn)
{
	const struct react_context *react;

	if (!turn || !turn->runtime.react)
		return 0;
	react = turn->runtime.react;
	/* A failed checkpoint must not discard work already performed this turn. */
	if (react->in_turn_compaction_count > 0 || react->hook_continuation_count > 0)
		return 1;
	if (!turn_has_error_code(turn, MORPH_ERR_NOT_CONFIGURED))
		return 0;
	return react->outcome == REACT_OUTCOME_LLM_ERROR ||
		react->outcome == REACT_OUTCOME_MAX_ITERATIONS ||
		react->outcome == REACT_OUTCOME_TOOL_ERROR ||
		react->outcome == REACT_OUTCOME_TIMEOUT;
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
		return -EINVAL;
	if (turn->finished)
		return -EALREADY;
	step_rc = turn_validate_runtime(&turn->runtime);
	if (step_rc != 0)
		return step_rc;

	memset(&local_result, 0, sizeof(local_result));
	flags = turn_flags(&turn->runtime);
	local_result.history_loaded =
		(flags & AGENT_TURN_LOAD_HISTORY) ? 1 : 0;
	local_result.memory_reply_prepared = (flags & AGENT_TURN_PREPARE_MEMORY_REPLY) ? 1 : 0;

	if (flags & AGENT_TURN_SAVE_TRACE) {
		step_rc = turn_save_trace(turn);
		local_result.trace_rc = step_rc;
		if (step_rc == 0)
			local_result.trace_saved =
				turn->runtime.react->steps ? 1 : 0;
	}
	if ((flags & AGENT_TURN_SAVE_MESSAGES) &&
	    !turn_model_history_succeeded(turn) &&
	    !turn_should_keep_complete_history(turn)) {
		if (turn_should_keep_user_history(turn)) {
			step_rc = model_history_deactivate_turn_except_user(
				turn->runtime.db, turn->runtime.session_id,
				turn->runtime.react->turn_id);
		} else {
			step_rc = model_history_deactivate_turn(
				turn->runtime.db, turn->runtime.session_id,
				turn->runtime.react->turn_id);
		}
		turn_set_first_error(&rc, step_rc);
	}

	if (flags & AGENT_TURN_SAVE_MESSAGES) {
		step_rc = db_exec(turn->runtime.db, "BEGIN IMMEDIATE;");
		if (step_rc == 0) {
			step_rc = turn_persist_user(turn, &local_result);
			local_result.user_rc = step_rc;
		}
		if (step_rc == 0) {
			step_rc = turn_persist_assistant(
				turn, &local_result, &assistant_for_memory);
			local_result.assistant_rc = step_rc;
		}
		if (step_rc == 0 && (turn_model_history_succeeded(turn) ||
		    turn->runtime.react->terminal_tool_error < 0) &&
		    turn->runtime.react->final_answer &&
		    turn->runtime.react->final_answer[0])
			step_rc = agent_history_record_assistant(
				turn->runtime.react,
				turn->runtime.react->final_answer);
		if (step_rc == 0)
			step_rc = agent_history_record_turn_outcome(turn->runtime.react);
		if (step_rc == 0)
			step_rc = db_exec(turn->runtime.db, "COMMIT;");
		if (step_rc != 0) {
			(void)db_exec(turn->runtime.db, "ROLLBACK;");
			local_result.user_saved = 0;
			local_result.assistant_saved = 0;
			local_result.user_tokens = 0;
			local_result.assistant_tokens = 0;
			free(assistant_for_memory);
			assistant_for_memory = NULL;
			if (local_result.user_rc == 0)
				local_result.user_rc = step_rc;
			if (local_result.assistant_rc == 0)
				local_result.assistant_rc = step_rc;
			local_result.message_persistence_rc = step_rc;
			turn_set_first_error(&rc, step_rc);
		}
	}

	memory_answer = assistant_for_memory ?
		assistant_for_memory : turn->runtime.react->final_answer;
	step_rc = turn_save_memory(turn, &local_result, memory_answer);
	if (step_rc != 0 && local_result.memory_rc == 0)
		local_result.memory_rc = step_rc;

	free(assistant_for_memory);
	turn_unbind_history(turn->runtime.react);
	turn->finished = 1;
	if (result)
		*result = local_result;
	return rc;
}
