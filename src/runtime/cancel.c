#include "runtime/engine.h"

void runtime_cancel(struct runtime_engine *engine)
{
	struct react_context *react = runtime_engine_active_react(engine);

	if (react)
		react_cancel(react);
}
