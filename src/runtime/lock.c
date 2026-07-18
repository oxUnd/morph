#include "runtime/runtime_internal.h"

#include <errno.h>

int runtime_lock_turn(struct runtime_engine *engine,
		      const struct runtime_request *request,
		      struct runtime_result *result)
{
	if (!engine->execution_lock)
		return 0;
	if (pthread_mutex_trylock(engine->execution_lock) == 0)
	{
		engine->turn_state.active = 1;
		engine->turn_state.session_id = request ? request->session_id : 0;
		engine->turn_state.react = engine->react;
		return 0;
	}
	result->outcome = RUNTIME_OUTCOME_BUSY;
	result->execution_rc = -EBUSY;
	return -EBUSY;
}

void runtime_unlock_turn(struct runtime_engine *engine)
{
	engine->turn_state.active = 0;
	engine->turn_state.session_id = 0;
	engine->turn_state.react = NULL;
	if (engine->execution_lock)
		pthread_mutex_unlock(engine->execution_lock);
}
