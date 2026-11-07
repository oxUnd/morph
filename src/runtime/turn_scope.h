#ifndef MORPH_RUNTIME_TURN_SCOPE_H
#define MORPH_RUNTIME_TURN_SCOPE_H

#include "agent/plan.h"
#include "agent/react.h"
#include "agent/tools/scheduled_tasks.h"
#include "agent/tool.h"
#include "runtime/context.h"
#include "runtime/request.h"
#include "../session.h"

#include <stddef.h>

struct runtime_engine;
struct hook_manager;

struct runtime_turn_scope {
	struct runtime_engine *engine;
	morph_event_cb event_cb;
	void *event_user_data;
	hitl_approval_cb hitl_cb;
	void *hitl_user_data;
	ask_user_callback_fn ask_user_fn;
	void *ask_user_data;
	int (*prompt_pending_fn)(void *);
	react_action_drain_fn action_drain_fn;
	void *action_drain_user_data;
	tool_operation_approval_fn operation_approval_fn;
	void *operation_approval_user_data;
	int request_bound;
	int request_usage_bound;

	struct model_usage_sink *usage_sink;
	void *previous_usage_user_data;
	int bound;
	int64_t memory_session_id;
	char credit_session_id[64];
	char user_id[64];
};

struct runtime_turn_scope_context {
	struct db *db;
	const struct config *config;
	struct tool_registry *tools;
	struct react_context *react;
	struct session *current_session;
	struct runtime_plan_session *plan_sessions;
	int plan_session_count;
	int64_t *active_plan_session_id;
	struct plan_registry *active_plans;
	void *usage_user_data;
	struct runtime_turn_scope *scope;
};

struct memory_options runtime_memory_options_from_config(const struct config *config);
void runtime_credit_session_key(const struct runtime_turn_scope *scope,
				const struct session *current_session, char *buf, size_t buf_size);
/* Request callbacks close only after all borrowed tool workers are drained. */
void runtime_turn_scope_bind_request(struct runtime_turn_scope *scope,
				     struct runtime_engine *engine,
				     const struct runtime_request *request);
void runtime_turn_scope_restore_request(struct runtime_turn_scope *scope);
int runtime_turn_scope_start_hooks(struct runtime_engine *engine,
				   const struct runtime_request *request);
void runtime_turn_scope_close_hooks(struct runtime_engine *engine);
int runtime_turn_scope_begin(struct runtime_turn_scope_context *ctx,
			     const struct runtime_request *request);
void runtime_turn_scope_finish(struct runtime_turn_scope_context *ctx);

#endif
