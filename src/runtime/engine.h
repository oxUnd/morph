#ifndef MORPH_RUNTIME_ENGINE_H
#define MORPH_RUNTIME_ENGINE_H

#include "agent/turn.h"
#include "runtime/request.h"
#include "runtime/result.h"
#include <pthread.h>
#include <stdint.h>

struct runtime_turn_state {
	int active;
	int64_t session_id;
	struct react_context *react;
};

typedef int (*runtime_prepare_turn_fn)(void *user_data,
					       const struct runtime_request *request);
typedef void (*runtime_finish_turn_fn)(void *user_data,
					       const struct runtime_request *request,
					       const struct runtime_result *result);

struct runtime_engine {
	struct db *db;
	struct react_context *react;
	const struct memory_options *memory_options;
	agent_turn_background_cb background_cb;
	void *background_user_data;
	pthread_mutex_t *execution_lock;
	struct runtime_turn_state turn_state;
	runtime_prepare_turn_fn prepare_turn;
	runtime_finish_turn_fn finish_turn;
	void *user_data;
};

void runtime_engine_init(struct runtime_engine *engine);
void runtime_engine_configure(struct runtime_engine *engine,
			      struct db *db,
			      struct react_context *react,
			      pthread_mutex_t *execution_lock);
void runtime_engine_cleanup(struct runtime_engine *engine);
struct react_context *runtime_engine_active_react(struct runtime_engine *engine);

#endif
