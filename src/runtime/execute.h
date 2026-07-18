#ifndef MORPH_RUNTIME_EXECUTE_H
#define MORPH_RUNTIME_EXECUTE_H

#include "runtime/engine.h"

int runtime_execute(struct runtime_engine *engine,
		    const struct runtime_request *request,
		    struct runtime_result *result);
void runtime_cancel(struct runtime_engine *engine);

#endif
