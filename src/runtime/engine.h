#ifndef MORPH_RUNTIME_ENGINE_H
#define MORPH_RUNTIME_ENGINE_H

struct nb_log;
#include "agent/turn.h"
#include "runtime/request.h"
#include "runtime/result.h"
#include "nb/thread.h"
#include <stdint.h>

struct runtime_turn_state {
	int active;
	int64_t session_id;
	struct react_context *react;
};

typedef int (*runtime_prepare_turn_fn)(void *user_data, const struct runtime_request *request);
typedef void (*runtime_finish_turn_fn)(void *user_data, const struct runtime_request *request,
				       const struct runtime_result *result);
typedef void (*runtime_drain_turn_fn)(void *user_data);

struct hook_manager;
struct runtime_turn_scope;

struct runtime_engine {
	struct nb_log *logger;
	struct tool_context *tool_context;
	struct hook_manager *hooks;
	struct db *db;
	struct react_context *react;
	const struct config *config;
	const struct memory_options *memory_options;
	agent_turn_background_cb background_cb;
	void *background_user_data;
	nb_mutex *execution_lock;
	struct runtime_turn_state turn_state;
	struct runtime_turn_scope *turn_scope;
	runtime_prepare_turn_fn prepare_turn;
	runtime_finish_turn_fn finish_turn;
	runtime_drain_turn_fn drain_turn;
	void *user_data;
};

void runtime_engine_init(struct runtime_engine *engine);
void runtime_engine_configure(struct runtime_engine *engine, struct db *db,
			      struct react_context *react, nb_mutex *execution_lock);
void runtime_engine_cleanup(struct runtime_engine *engine);
struct react_context *runtime_engine_active_react_borrow(struct runtime_engine *engine);

#endif
