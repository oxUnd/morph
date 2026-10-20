#ifndef MORPH_AGENT_HOOKS_H
#define MORPH_AGENT_HOOKS_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returned JSON is owned by the caller; decisions are encoded in that object. */
typedef int (*agent_hook_fn)(void *user, const char *event, const cJSON *input, cJSON **output);

#ifdef __cplusplus
}
#endif
#endif
