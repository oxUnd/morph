#ifndef MORPH_PROJECT_CONTEXT_H
#define MORPH_PROJECT_CONTEXT_H

#include "environment_context.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROJECT_CONTEXT_MAX_BYTES (32 * 1024)

/* Optional global directory, then Git root through cwd; turn-arena output. */
int project_context_build(const struct environment_context *environment,
	const char *global_dir, struct arena *arena, const char **out);

#ifdef __cplusplus
}
#endif
#endif
