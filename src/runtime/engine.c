#include "runtime/engine.h"

#include <stddef.h>

void runtime_engine_configure(struct runtime_engine *engine, struct db *db,
			      struct react_context *react, nb_mutex *execution_lock)
{
	if (!engine)
		return;
	engine->logger = react ? react->logger : NULL;
	engine->db = db;
	engine->react = react;
	engine->execution_lock = execution_lock;
}

struct react_context *runtime_engine_active_react_borrow(struct runtime_engine *engine)
{
	if (!engine)
		return NULL;
	/* The engine owns one stable ReAct context. Cancellation must not read
	 * execution-thread-only turn bookkeeping without synchronization. */
	return engine->react;
}
