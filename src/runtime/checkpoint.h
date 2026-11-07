#ifndef MORPH_RUNTIME_CHECKPOINT_H
#define MORPH_RUNTIME_CHECKPOINT_H

#include "cJSON.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct runtime;

enum runtime_rewind_mode {
	RUNTIME_REWIND_CODE = 1,
	RUNTIME_REWIND_CONVERSATION = 2,
	RUNTIME_REWIND_BOTH = 3,
};

/* Idle-session operations. Output slots start NULL and receive owned JSON.
 * Restore can return a report on failure; callers always release it.
 * Files changed outside tracked editing tools cause a conflict before writes. */
int runtime_checkpoint_list_copy(struct runtime *runtime, cJSON **out);
int runtime_checkpoint_preview_copy(struct runtime *runtime, int64_t id, cJSON **out);
int runtime_checkpoint_restore(struct runtime *runtime, int64_t id, enum runtime_rewind_mode mode,
			       cJSON **out);

#ifdef __cplusplus
}
#endif

#endif
