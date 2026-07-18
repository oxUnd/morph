#include "runtime/engine.h"

#include <string.h>

void runtime_engine_init(struct runtime_engine *engine)
{
	if (engine)
		memset(engine, 0, sizeof(*engine));
}

void runtime_engine_configure(struct runtime_engine *engine,
			      struct db *db,
			      struct react_context *react,
			      pthread_mutex_t *execution_lock)
{
	if (!engine)
		return;
	engine->db = db;
	engine->react = react;
	engine->execution_lock = execution_lock;
}

void runtime_engine_cleanup(struct runtime_engine *engine)
{
	if (engine)
		memset(&engine->turn_state, 0, sizeof(engine->turn_state));
}

struct react_context *runtime_engine_active_react(struct runtime_engine *engine)
{
	if (!engine)
		return NULL;
	return engine->turn_state.active && engine->turn_state.react
		? engine->turn_state.react
		: engine->react;
}
