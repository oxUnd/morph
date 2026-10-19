#ifndef MORPH_SANDBOX_TOOL_PATHS_H
#define MORPH_SANDBOX_TOOL_PATHS_H

#include "sandbox.h"
#include "util/arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Extend an exec policy. The arena owns the resulting path arrays. */
int sandbox_prepare_tool_paths(struct sandbox_config *cfg,
			       struct arena *arena);

#ifdef __cplusplus
}
#endif
#endif
