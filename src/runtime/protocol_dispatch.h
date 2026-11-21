#ifndef MORPH_RUNTIME_PROTOCOL_DISPATCH_H
#define MORPH_RUNTIME_PROTOCOL_DISPATCH_H

#include "cJSON.h"
#include <stdint.h>

struct runtime;

/* The caller supplies the session actor for session-scoped operations. Outputs
 * own their complete data and remain valid after actor unload. */
int runtime_protocol_dispatch(struct runtime *runtime, const char *method,
	const cJSON *params, cJSON **out);
int runtime_protocol_session_copy(struct runtime *runtime, int64_t id, cJSON **out);

#endif
