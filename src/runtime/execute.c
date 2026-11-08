#include "runtime/execute.h"
#include "util/signals.h"
#include "runtime/runtime_internal.h"
#include "runtime/output.h"
#include "util/log.h"
#include "util/error.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>

struct runtime_output_forwarder {
	const struct runtime_request *request;
	struct runtime_engine *engine;
};

static int runtime_output_forward(const struct react_output_event *event, void *user_data)
{
	struct runtime_output_forwarder *forwarder = user_data;
	struct runtime_output_context output;
	int rc;

	memset(&output, 0, sizeof(output));
	output.db = forwarder->engine->db;
	output.config = forwarder->engine->config;
	output.session_id = forwarder->request->session_id;
	output.request_prompt = forwarder->request->stored_user_input;
	output.turn_id = forwarder->engine->react->turn_id;
	output.event_cb = forwarder->engine->react->event_cb;
	output.event_user_data = forwarder->engine->react->event_user_data;
	rc = runtime_output_record_event(&output, event);
	if (rc != 0)
		log_warn(forwarder->engine->logger, "failed to record generation output: %s",
			 morph_strerror(rc));
	if (forwarder->request->output_cb)
		return forwarder->request->output_cb(event, forwarder->request->output_user_data);
	return 0;
}

static int runtime_execute_locked(struct runtime_engine *engine,
	const struct runtime_request *request, struct runtime_result *result,
	struct runtime_turn_scope *scope)
{
	struct agent_session_runtime session_runtime;
	struct agent_turn turn;
	struct runtime_output_forwarder output_forwarder;
	struct memory_options memory = {0};
	int rc;
	int finish_rc;

	runtime_turn_scope_bind_request(scope, engine, request);
	memset(&session_runtime, 0, sizeof(session_runtime));
	session_runtime.db = engine->db;
	session_runtime.session_id = request->session_id;
	session_runtime.react = engine->react;
	session_runtime.memory_options =
		request->memory_options ? request->memory_options : engine->memory_options;
	if (session_runtime.memory_options) {
		memory = *session_runtime.memory_options;
		if (request->restrict_memory_to_user) {
			memory.visible_fn = request->memory_visible_fn;
			memory.visible_user_id = request->user_id;
			memory.visible_user_data = request->memory_visible_user_data;
		}
		session_runtime.memory_options = &memory;
	}
	session_runtime.render_assistant = request->render_assistant;
	session_runtime.render_user_data = request->render_user_data;
	session_runtime.background_cb = engine->background_cb;
	session_runtime.background_user_data = engine->background_user_data;
	session_runtime.flags =
		request->turn_flags ? request->turn_flags : AGENT_TURN_DEFAULT_FLAGS;
	memset(&turn, 0, sizeof(turn));
	morph_signal_flag_set(&engine->react->cancelled, 0);
	morph_cancel_token_reset(&engine->react->cancel_token);
	rc = 0;
	rc = runtime_turn_scope_start_hooks(engine, request);
	if (rc == 0) {
		rc = agent_turn_begin(&turn, &session_runtime,
				      &(struct agent_turn_input){
					      .model_input = request->model_input,
					      .stored_user_input = request->stored_user_input,
					      .turn_id = request->turn_id,
				      });
		if (rc < 0) {
			char *detail = engine->react->last_error_detail;

			engine->react->last_error_detail = NULL;
			react_reset(engine->react);
			engine->react->last_error_detail = detail;
			engine->react->outcome = rc == -ECANCELED ? REACT_OUTCOME_CANCELLED
						 : rc == -EPERM	  ? REACT_OUTCOME_GUARDRAIL_DENIED
								  : REACT_OUTCOME_INTERNAL_ERROR;
			engine->react->state = REACT_STATE_ABORT;
			engine->react->last_error_code = rc;
		}
	}
	if (rc == 0) {
		output_forwarder.request = request;
		output_forwarder.engine = engine;
		rc = react_run(engine->react, request->model_input, runtime_output_forward,
			       &output_forwarder);
	} else if (rc == -ECANCELED && engine->hooks) {
		cJSON *output = NULL;

		(void)hook_manager_dispatch(engine->hooks, "Interrupt", NULL, &output);
		cJSON_Delete(output);
	}
	react_drain_tools(engine->react);
	runtime_turn_scope_close_hooks(engine);
	if (turn.begun) {
		finish_rc = agent_turn_finish(&turn, &result->turn);
		result->persistence_rc = finish_rc;
		if (rc == 0 && finish_rc != 0)
			rc = finish_rc;
	}
	result->final_text = engine->react->final_answer;
	if (engine->react->outcome == REACT_OUTCOME_CANCELLED)
		result->outcome = RUNTIME_OUTCOME_CANCELLED;
	else if (rc == 0)
		result->outcome = RUNTIME_OUTCOME_COMPLETED;
	result->execution_rc = rc;

	return rc;
}

int runtime_execute(struct runtime_engine *engine, const struct runtime_request *request,
		    struct runtime_result *result)
{
	struct runtime_turn_scope local_scope = {0};
	struct runtime_turn_scope *scope;
	int rc;

	if (!engine || !engine->db || !engine->react || !request || request->session_id <= 0 ||
	    !request->model_input || !result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	result->outcome = RUNTIME_OUTCOME_FAILED;
	rc = runtime_lock_turn(engine, request, result);
	if (rc != 0)
		return rc;
	react_drain_tools(engine->react);
	scope = engine->turn_scope ? engine->turn_scope : &local_scope;
	if (engine->prepare_turn)
		rc = engine->prepare_turn(engine->user_data, request);

	if (rc == 0)
		rc = runtime_execute_locked(engine, request, result, scope);

	if (result->execution_rc == 0 && result->outcome != RUNTIME_OUTCOME_COMPLETED)
		result->execution_rc = rc;
	runtime_turn_scope_restore_request(scope);
	if (engine->finish_turn)
		engine->finish_turn(engine->user_data, request, result);
	runtime_unlock_turn(engine);
	return rc;
}
