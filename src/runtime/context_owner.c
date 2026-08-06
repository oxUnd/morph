#include "runtime/context_owner.h"

#include "runtime/bootstrap.h"
#include "runtime/turn_scope.h"

#include <errno.h>
#include <string.h>

void runtime_context_init_empty(struct runtime_context *ctx)
{
	if (!ctx)
		return;
	memset(ctx, 0, sizeof(*ctx));
}

int runtime_context_init_lock(struct runtime_context *ctx)
{
	int rc;

	if (!ctx)
		return -EINVAL;
	rc = pthread_mutex_init(&ctx->execution_lock, NULL);
	if (rc != 0)
		return -rc;
	ctx->execution_lock_ready = 1;
	return 0;
}

void runtime_context_cleanup_lock(struct runtime_context *ctx)
{
	if (!ctx || !ctx->execution_lock_ready)
		return;
	pthread_mutex_destroy(&ctx->execution_lock);
	ctx->execution_lock_ready = 0;
}

void runtime_context_configure_engine(struct runtime_context *ctx)
{
	if (!ctx)
		return;
	runtime_engine_configure(&ctx->engine, &ctx->database, ctx->react,
				 &ctx->execution_lock);
	ctx->engine.config = &ctx->config;
}

struct runtime_models runtime_context_models(struct runtime_context *ctx)
{
	struct runtime_models models;

	memset(&models, 0, sizeof(models));
	if (!ctx)
		return models;
	models.tokenizer = ctx->tokenizer;
	models.react = ctx->react;
	models.text = ctx->llm;
	models.vision = ctx->vision_llm;
	models.image = ctx->img_llm;
	models.video = ctx->vid_llm;
	return models;
}

void runtime_context_update_models(struct runtime_context *ctx,
				   const struct runtime_models *models)
{
	if (!ctx || !models)
		return;
	ctx->tokenizer = models->tokenizer;
	ctx->react = models->react;
	ctx->llm = models->text;
	ctx->vision_llm = models->vision;
	ctx->img_llm = models->image;
	ctx->vid_llm = models->video;
}

void runtime_context_select_plan_session(struct runtime_context *ctx,
					 int64_t session_id)
{
	if (!ctx)
		return;
	runtime_plan_session_select(ctx->plan_sessions,
				    RUNTIME_PLAN_SESSION_CACHE_MAX,
				    &ctx->active_plan_session_id,
				    &ctx->plans, session_id);
}

void runtime_context_forget_plan_session(struct runtime_context *ctx,
					 int64_t session_id)
{
	if (!ctx)
		return;
	runtime_plan_session_forget(ctx->plan_sessions,
				    RUNTIME_PLAN_SESSION_CACHE_MAX,
				    &ctx->active_plan_session_id,
				    &ctx->plans, session_id);
}

int runtime_context_update_tool_runtime_context(struct runtime_context *ctx,
					       int64_t session_id)
{
	struct tool_runtime_context rt;
	int rc;

	if (!ctx || !ctx->react)
		return -EINVAL;
	rc = runtime_tool_context_for_session(&ctx->database, &ctx->config,
					      &ctx->current_session,
					      session_id, &rt);
	if (rc != 0)
		return rc;
	react_set_tool_runtime_context(ctx->react, &rt);
	return 0;
}

int runtime_context_tool_credit_session_id(struct runtime_context *ctx,
					   int64_t session_id,
					   char *buf, size_t buf_size)
{
	struct tool_runtime_context rt;
	int rc;

	if (!ctx || !buf || buf_size == 0)
		return -EINVAL;
	rc = runtime_tool_context_for_session(&ctx->database, &ctx->config,
					      &ctx->current_session,
					      session_id, &rt);
	if (rc != 0)
		return rc;
	strncpy(buf, rt.credit_session_id, buf_size - 1);
	buf[buf_size - 1] = '\0';
	return 0;
}

struct memory_options
runtime_context_memory_options(const struct runtime_context *ctx)
{
	return runtime_memory_options_from_config(ctx ? &ctx->config : NULL);
}

void runtime_context_credit_session_key(struct runtime_context *ctx,
					char *buf, size_t buf_size)
{
	if (!buf || buf_size == 0)
		return;
	if (!ctx) {
		buf[0] = '\0';
		return;
	}
	runtime_credit_session_key(&ctx->turn_scope, &ctx->current_session,
				   buf, buf_size);
}

int runtime_context_prepare_turn(struct runtime_context *ctx,
				 const struct runtime_request *request,
				 void *usage_user_data)
{
	struct runtime_turn_scope_context scope;

	if (!ctx || !request || request->session_id <= 0)
		return -EINVAL;
	if (ctx->sub_agents)
		(void)sub_agent_runtime_set_parent_session(ctx->sub_agents,
						   request->session_id);
	memset(&scope, 0, sizeof(scope));
	scope.db = &ctx->database;
	scope.config = &ctx->config;
	scope.tools = &ctx->tools;
	scope.react = ctx->react;
	scope.current_session = &ctx->current_session;
	scope.plan_sessions = ctx->plan_sessions;
	scope.plan_session_count = RUNTIME_PLAN_SESSION_CACHE_MAX;
	scope.active_plan_session_id = &ctx->active_plan_session_id;
	scope.active_plans = &ctx->plans;
	scope.usage_user_data = usage_user_data;
	scope.scope = &ctx->turn_scope;
	return runtime_turn_scope_begin(&scope, request);
}

void runtime_context_finish_turn(struct runtime_context *ctx)
{
	struct runtime_turn_scope_context scope;

	if (!ctx)
		return;
	memset(&scope, 0, sizeof(scope));
	scope.db = &ctx->database;
	scope.config = &ctx->config;
	scope.tools = &ctx->tools;
	scope.react = ctx->react;
	scope.current_session = &ctx->current_session;
	scope.plan_sessions = ctx->plan_sessions;
	scope.plan_session_count = RUNTIME_PLAN_SESSION_CACHE_MAX;
	scope.active_plan_session_id = &ctx->active_plan_session_id;
	scope.active_plans = &ctx->plans;
	scope.scope = &ctx->turn_scope;
	runtime_turn_scope_finish(&scope);
}

struct runtime_shutdown_resources
runtime_context_shutdown_resources(struct runtime_context *ctx,
				   int shutdown_memory,
				   int reset_usage_callbacks,
				   int free_skill_registry)
{
	struct runtime_shutdown_resources cleanup;

	memset(&cleanup, 0, sizeof(cleanup));
	if (!ctx)
		return cleanup;
	cleanup.db = &ctx->database;
	cleanup.tools = &ctx->tools;
	cleanup.tool_context = &ctx->tctx;
	cleanup.skills = &ctx->skills;
	cleanup.mcp = &ctx->mcp;
	cleanup.sub_agents = &ctx->sub_agents;
	cleanup.react = &ctx->react;
	cleanup.tokenizer = &ctx->tokenizer;
	cleanup.text = &ctx->llm;
	cleanup.vision = &ctx->vision_llm;
	cleanup.image = &ctx->img_llm;
	cleanup.video = &ctx->vid_llm;
	cleanup.shutdown_memory = shutdown_memory;
	cleanup.reset_usage_callbacks = reset_usage_callbacks;
	cleanup.free_skill_registry = free_skill_registry;
	return cleanup;
}
