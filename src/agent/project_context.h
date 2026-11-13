#ifndef MORPH_PROJECT_CONTEXT_H
#define MORPH_PROJECT_CONTEXT_H

#include <stddef.h>

struct nb_log;
#include "environment_context.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROJECT_CONTEXT_MAX_BYTES (32 * 1024)

/* Optional global directory, then Git root through cwd; caller-owned output. */
int project_context_create(struct nb_log *logger, const struct environment_context *environment,
			   const char *global_dir, char **out);
int project_context_create_with_budget(struct nb_log *logger,
				       const struct environment_context *environment,
				       const char *global_dir, size_t budget, char **out);

#ifdef __cplusplus
}
#endif
#endif
