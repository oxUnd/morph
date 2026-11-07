#include "runtime/turn_scope.h"

#include "agent/tools/dynamic_tools.h"
#include "runtime/usage.h"
#include "runtime/engine.h"
#include "runtime/hooks.h"
#include "util/error.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct memory_options runtime_memory_options_from_config(const struct config *config)
{
	struct memory_options opts;

	memset(&opts, 0, sizeof(opts));
	if (!config)
		return opts;
	opts.enabled = config->memory.enabled;
	opts.hot_path_enabled = config->memory.hot_path_enabled;
	opts.cold_path_enabled = config->memory.cold_path_enabled;
	opts.llm_extract_enabled = config->memory.llm_extract_enabled;
	opts.max_facts = config->memory.max_facts;
	opts.max_episodes = config->memory.max_episodes;
	opts.max_procedures = config->memory.max_procedures;
	opts.max_context_chars = config->memory.max_context_chars;
	return opts;
}

void runtime_credit_session_key(const struct runtime_turn_scope *scope,
				const struct session *current_session, char *buf, size_t buf_size)
{
	if (!buf || buf_size == 0)
		return;
	if (!current_session) {
		buf[0] = '\0';
		return;
	}
	if (scope && scope->bound && scope->credit_session_id[0]) {
		snprintf(buf, buf_size, "%s", scope->credit_session_id);
		return;
	}
	if (current_session->display_id[0]) {
		snprintf(buf, buf_size, "%s", current_session->display_id);
		return;
	}
	snprintf(buf, buf_size, "%lld", (long long)current_session->id);
}

static void runtime_turn_scope_credit_session_id(struct db *db,
						 const struct session *current_session,
						 int64_t session_id, char *buf, size_t buf_size)
{
	struct session s;
	const char *credit_session_id = NULL;

	if (!buf || buf_size == 0)
		return;
	buf[0] = '\0';
	if (current_session && session_id == current_session->id) {
		credit_session_id = current_session->display_id[0] ? current_session->display_id
								   : current_session->name;
	} else if (db && session_id > 0 && session_get_by_id(db, session_id, &s) == 0) {
		session_ensure_display_id(db, &s);
		credit_session_id = s.display_id[0] ? s.display_id : s.name;
	}
	if (credit_session_id)
		snprintf(buf, buf_size, "%s", credit_session_id);
}

static void runtime_turn_scope_bind_tool_context(struct runtime_turn_scope_context *ctx,
						 const struct runtime_request *request)
{
	struct tool_runtime_context rt;
	int64_t session_id;

	if (!ctx || !ctx->react || !request)
		return;
	session_id = request->session_id;
	if (runtime_tool_context_for_session(ctx->db, ctx->config, ctx->current_session, session_id,
					     &rt) != 0)
		return;
	if (request->user_id && request->user_id[0])
		rt.user_id = request->user_id;
	rt.restrict_memory_to_user = request->restrict_memory_to_user;
	rt.memory_visible_fn = request->memory_visible_fn;
	rt.memory_visible_user_data = request->memory_visible_user_data;
	if (ctx->checkpoint)
		rt.checkpoint = *ctx->checkpoint;
	react_set_tool_runtime_context(ctx->react, &rt);
	if (ctx->tools)
		(void)dynamic_tools_set_session_id(ctx->tools, rt.credit_session_id);
}

int runtime_turn_scope_begin(struct runtime_turn_scope_context *ctx,
			     const struct runtime_request *request)
{
	if (!ctx || !ctx->scope || !ctx->db || !ctx->config || !request || request->session_id <= 0)
		return -EINVAL;
	{
		int rc = preference_bind(ctx->db, request->session_id,
					 request->user_id && request->user_id[0] ? request->user_id
										 : "local",
					 ctx->react ? ctx->react->workdir : NULL);

		if (rc != 0)
			return rc;
	}
	ctx->scope->usage_sink =
		ctx->react && ctx->react->llm_model ? ctx->react->llm_model->usage_sink : NULL;
	ctx->scope->previous_usage_user_data = runtime_usage_bind(
		ctx->scope->usage_sink,
		request->bind_usage_user_data ? request->usage_user_data : ctx->usage_user_data);
	ctx->scope->bound = 1;
	ctx->scope->memory_session_id = request->session_id;
	if (request->user_id && request->user_id[0])
		snprintf(ctx->scope->user_id, sizeof(ctx->scope->user_id), "%s", request->user_id);
	else
		snprintf(ctx->scope->user_id, sizeof(ctx->scope->user_id), "local");
	runtime_turn_scope_credit_session_id(ctx->db, ctx->current_session, request->session_id,
					     ctx->scope->credit_session_id,
					     sizeof(ctx->scope->credit_session_id));
	if (ctx->tools)
		(void)scheduled_tasks_tool_set_time_anchor(ctx->tools, (int64_t)time(NULL));
	if (ctx->tools)
		(void)scheduled_tasks_tool_set_source_session(ctx->tools, request->session_id);
	if (ctx->plan_sessions && ctx->active_plan_session_id && ctx->active_plans) {
		runtime_plan_session_select(ctx->plan_sessions, ctx->plan_session_count,
					    ctx->active_plan_session_id, ctx->active_plans,
					    request->session_id);
	}
	if (ctx->active_plans) {
		int rc = plan_registry_bind(ctx->active_plans, ctx->db, request->session_id);
		if (rc != 0) {
			runtime_turn_scope_finish(ctx);
			return rc;
		}
		if (ctx->react)
			ctx->react->plans = ctx->active_plans;
	}
	runtime_turn_scope_bind_tool_context(ctx, request);
	return 0;
}

void runtime_turn_scope_finish(struct runtime_turn_scope_context *ctx)
{
	struct runtime_turn_scope *scope;

	if (!ctx || !ctx->scope || !ctx->scope->bound)
		return;
	scope = ctx->scope;
	runtime_usage_restore(scope->usage_sink, scope->previous_usage_user_data);
	scope->previous_usage_user_data = NULL;
	scope->bound = 0;
	scope->memory_session_id = 0;
	scope->credit_session_id[0] = '\0';
	scope->user_id[0] = '\0';
	if (ctx->current_session && ctx->plan_sessions && ctx->active_plan_session_id &&
	    ctx->active_plans) {
		runtime_plan_session_select(ctx->plan_sessions, ctx->plan_session_count,
					    ctx->active_plan_session_id, ctx->active_plans,
					    ctx->current_session->id);
	}
	if (ctx->current_session) {
		struct runtime_request request;

		memset(&request, 0, sizeof(request));
		request.session_id = ctx->current_session ? ctx->current_session->id : 0;
		if (request.session_id > 0)
			runtime_turn_scope_bind_tool_context(ctx, &request);
	}
}

void runtime_turn_scope_bind_request(struct runtime_turn_scope *scope,
				     struct runtime_engine *engine,
				     const struct runtime_request *request)
{
	struct react_context *react = engine->react;

	scope->engine = engine;
	scope->event_cb = react->event_cb;
	scope->event_user_data = react->event_user_data;
	scope->hitl_cb = react->hitl.approval_cb;
	scope->hitl_user_data = react->hitl.approval_user_data;
	scope->ask_user_fn = react->ask_user_fn;
	scope->ask_user_data = react->ask_user_data;
	scope->prompt_pending_fn = react->prompt_pending_fn;
	scope->action_drain_fn = react->action_drain_fn;
	scope->action_drain_user_data = react->action_drain_user_data;
	if (engine->tool_context)
		tool_context_operation_approval_borrow(engine->tool_context,
						       &scope->operation_approval_fn,
						       &scope->operation_approval_user_data);
	if (!scope->bound && request->bind_usage_user_data) {
		scope->usage_sink = react->llm_model ? react->llm_model->usage_sink : NULL;
		scope->previous_usage_user_data =
			runtime_usage_bind(scope->usage_sink, request->usage_user_data);
		scope->request_usage_bound = 1;
	}
	scope->request_bound = 1;
	if (request->event_cb)
		react_set_event_callback(react, request->event_cb, request->event_user_data);
	if (request->override_hitl) {
		react->hitl.approval_cb = request->hitl_cb;
		react->hitl.approval_user_data = request->hitl_user_data;
	}
	if (request->override_ask_user) {
		react->ask_user_fn = request->ask_user_fn;
		react->ask_user_data = request->ask_user_user_data;
	}
	if (request->override_operation_approval && engine->tool_context)
		tool_context_set_operation_approval(engine->tool_context,
						    request->operation_approval_fn,
						    request->operation_approval_user_data);
	if (request->override_action_drain) {
		react->prompt_pending_fn = request->prompt_pending_fn;
		react->action_drain_fn = request->action_drain_fn;
		react->action_drain_user_data = request->action_drain_user_data;
	}
}

void runtime_turn_scope_restore_request(struct runtime_turn_scope *scope)
{
	struct runtime_engine *engine;
	struct react_context *react;

	if (!scope || !scope->request_bound)
		return;
	engine = scope->engine;
	react = engine->react;
	react_set_event_callback(react, scope->event_cb, scope->event_user_data);
	react->hitl.approval_cb = scope->hitl_cb;
	react->hitl.approval_user_data = scope->hitl_user_data;
	react->ask_user_fn = scope->ask_user_fn;
	react->ask_user_data = scope->ask_user_data;
	react->prompt_pending_fn = scope->prompt_pending_fn;
	react->action_drain_fn = scope->action_drain_fn;
	react->action_drain_user_data = scope->action_drain_user_data;
	if (engine->tool_context)
		tool_context_set_operation_approval(engine->tool_context,
						    scope->operation_approval_fn,
						    scope->operation_approval_user_data);
	if (scope->request_usage_bound) {
		runtime_usage_restore(scope->usage_sink, scope->previous_usage_user_data);
		scope->previous_usage_user_data = NULL;
		scope->request_usage_bound = 0;
	}
	scope->request_bound = 0;
	scope->engine = NULL;
}

int runtime_turn_scope_start_hooks(struct runtime_engine *engine,
				   const struct runtime_request *request)
{
	int rc = 0;
	/* Bind before begin: loading history may compact before storing the prompt. */
	if (engine->hooks && engine->hooks->ready) {
		struct hook_manager *hooks = engine->hooks;
		cJSON *input = cJSON_CreateObject(), *output = NULL;
		rc = hook_manager_reload(hooks);
		hooks->remaining_ms = 600000;
		hooks->context_bytes = 0;
		hooks->event_cb = engine->react->event_cb;
		hooks->event_user = engine->react->event_user_data;
		hooks->turn_id = engine->react->turn_id;
		hooks->model = engine->config ? engine->config->models.text.model : "";
		hooks->cancelled = &engine->react->cancelled;
		hooks->cancel_token = &engine->react->cancel_token;
		if (rc == 0 && hooks->session_id != request->session_id) {
			const char *source = hooks->session_id ? "resume" : "startup";
			if (hooks->session_id) {
				cJSON_AddStringToObject(input, "reason", "other");
				rc = hook_manager_dispatch(hooks, "SessionEnd", input, &output);
				cJSON_Delete(output);
				output = NULL;
				cJSON_DeleteItemFromObject(input, "reason");
			}
			hooks->session_id = request->session_id;
			cJSON_AddStringToObject(input, "source", source);
			if (rc == 0)
				rc = hook_manager_dispatch(hooks, "SessionStart", input, &output);
			if (rc == 0 && cJSON_IsTrue(cJSON_GetObjectItem(output, "terminate")))
				rc = -EPERM;
			free(hooks->session_context);
			hooks->session_context = NULL;
			const cJSON *extra = cJSON_GetObjectItem(output, "context");
			if (cJSON_IsString(extra)) {
				hooks->session_context = strdup(extra->valuestring);
				if (!hooks->session_context)
					rc = -ENOMEM;
			}
		}
		cJSON_Delete(input);
		if (rc != 0) {
			react_reset(engine->react);
			const cJSON *reason = cJSON_GetObjectItem(output, "reason");
			if (cJSON_IsString(reason)) {
				engine->react->last_error_detail = strdup(reason->valuestring);
				if (!engine->react->last_error_detail)
					rc = -ENOMEM;
			}
			engine->react->outcome = rc == -ECANCELED ? REACT_OUTCOME_CANCELLED
						 : rc == -EPERM	  ? REACT_OUTCOME_GUARDRAIL_DENIED
								  : REACT_OUTCOME_INTERNAL_ERROR;
			engine->react->state = REACT_STATE_ABORT;
			engine->react->last_error_code = rc;
		}
		cJSON_Delete(output);
		engine->react->hook_fn = hook_manager_dispatch;
		engine->react->hook_user = hooks;
		engine->react->hook_stop_active = hook_manager_has(hooks, "Stop");
	}

	return rc;
}

void runtime_turn_scope_close_hooks(struct runtime_engine *engine)
{
	engine->react->hook_fn = NULL;
	engine->react->hook_user = NULL;
	engine->react->hook_stop_active = 0;
	if (engine->hooks) {
		engine->hooks->event_cb = NULL;
		engine->hooks->event_user = NULL;
		engine->hooks->cancelled = NULL;
		engine->hooks->cancel_token = NULL;
	}
}
