#ifndef MORPH_AGENT_HOOKS_H
#define MORPH_AGENT_HOOKS_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returned JSON is owned by the caller; decisions are encoded in that object. */
typedef int (*agent_hook_fn)(void *user, const char *event, const cJSON *input, cJSON **output);

struct react_context;
/* Compaction hooks run on the owner thread; a stop returns -EPERM. */
int agent_hook_compaction(struct react_context *ctx, const char *event, const char *trigger);

#ifdef __cplusplus
}
#endif
#endif
