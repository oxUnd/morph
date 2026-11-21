#ifndef MORPH_RUNTIME_PROTOCOL_INSPECT_H
#define MORPH_RUNTIME_PROTOCOL_INSPECT_H

#include "cJSON.h"

struct runtime;

int runtime_protocol_turn_inspect(struct runtime *runtime, cJSON **out);
int runtime_protocol_context_get(struct runtime *runtime, cJSON **out);
int runtime_protocol_history_manage(struct runtime *runtime, const char *method, cJSON **out);

#endif
