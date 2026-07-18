#ifndef MORPH_RUNTIME_CONTEXT_OWNER_H
#define MORPH_RUNTIME_CONTEXT_OWNER_H

#include "agent/plan.h"
#include "agent/react.h"
#include "agent/sub_agent.h"
#include "agent/tool_context.h"
#include "config.h"
#include "db/database.h"
#include "mcp/mcp.h"
#include "models/llm.h"
#include "runtime/bootstrap.h"
#include "runtime/context.h"
#include "runtime/engine.h"
#include "runtime/turn_scope.h"
#include "session.h"
#include "skill/skill.h"

#include <limits.h>
#include <pthread.h>
#include <stddef.h>

#define RUNTIME_PLAN_SESSION_CACHE_MAX 32

struct runtime_context {
	struct config config;
	struct db database;
	struct session current_session;
	struct tool_registry tools;
	struct plan_registry plans;
	struct runtime_plan_session plan_sessions[RUNTIME_PLAN_SESSION_CACHE_MAX];
	int64_t active_plan_session_id;
	struct skill_registry skill_storage;
	struct skill_registry *skills;
	struct react_context *react;
	struct tokenizer *tokenizer;
	struct model *llm;
	struct model *vision_llm;
	struct model *img_llm;
	struct model *vid_llm;
	struct mcp_registry mcp;
	struct tool_context *tctx;
	struct sub_agent_runtime *sub_agents;
	struct runtime_engine engine;
	struct runtime_turn_scope turn_scope;
	struct memory_options memory_options;
	pthread_mutex_t execution_lock;
	int execution_lock_ready;
	char config_path[PATH_MAX];
	char workdir[PATH_MAX];
};

void runtime_context_init_empty(struct runtime_context *ctx);
int runtime_context_init_lock(struct runtime_context *ctx);
void runtime_context_cleanup_lock(struct runtime_context *ctx);
void runtime_context_configure_engine(struct runtime_context *ctx);
struct runtime_models runtime_context_models(struct runtime_context *ctx);
void runtime_context_update_models(struct runtime_context *ctx,
				   const struct runtime_models *models);
void runtime_context_select_plan_session(struct runtime_context *ctx,
					 int64_t session_id);
void runtime_context_forget_plan_session(struct runtime_context *ctx,
					 int64_t session_id);
int runtime_context_update_tool_runtime_context(struct runtime_context *ctx,
					       int64_t session_id);
int runtime_context_tool_credit_session_id(struct runtime_context *ctx,
					   int64_t session_id,
					   char *buf, size_t buf_size);
struct memory_options
runtime_context_memory_options(const struct runtime_context *ctx);
void runtime_context_credit_session_key(struct runtime_context *ctx,
					char *buf, size_t buf_size);
int runtime_context_prepare_turn(struct runtime_context *ctx,
				 const struct runtime_request *request,
				 void *usage_user_data);
void runtime_context_finish_turn(struct runtime_context *ctx);
struct runtime_shutdown_resources
runtime_context_shutdown_resources(struct runtime_context *ctx,
				   int shutdown_memory,
				   int reset_usage_callbacks,
				   int free_skill_registry);

#endif
