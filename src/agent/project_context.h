#ifndef MORPH_PROJECT_CONTEXT_H
#define MORPH_PROJECT_CONTEXT_H

struct morph_log;
#include "environment_context.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROJECT_CONTEXT_MAX_BYTES (32 * 1024)

/* Optional global directory, then Git root through cwd; caller-owned output. */
int project_context_create(struct morph_log *logger, const struct environment_context *environment,
			   const char *global_dir, char **out);

#ifdef __cplusplus
}
#endif
#endif
