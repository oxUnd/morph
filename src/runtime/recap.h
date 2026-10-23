#ifndef MORPH_RUNTIME_RECAP_H
#define MORPH_RUNTIME_RECAP_H

#include "cJSON.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct runtime;

#define RECAP_MAX_CHARS 700
#define RECAP_NEXT_MAX_CHARS 200
#define RECAP_PROMPT_MAX_BYTES 32768

struct runtime_recap_progress {
	int64_t session_id;
	int64_t history_id;
	uint64_t revision;
	int completed_turns;
};

/* Nonblocking snapshots and requests return -EBUSY during foreground execution. */
int runtime_recap_progress(struct runtime *runtime, struct runtime_recap_progress *out);
int runtime_recap_request(struct runtime *runtime, int automatic,
	struct runtime_recap_progress *out);
void runtime_recap_cancel(struct runtime *runtime);
int runtime_recap_is_current(struct runtime *runtime,
	const struct runtime_recap_progress *progress);
/* Validate structured model output. Caller owns the returned JSON object. */
cJSON *runtime_recap_parse(const char *response);

#ifdef __cplusplus
}
#endif
#endif
