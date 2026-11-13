#ifndef MORPH_SANDBOX_TOOL_PATHS_H
#define MORPH_SANDBOX_TOOL_PATHS_H

#include "sandbox.h"
#include "nb/cleanup.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Extend an exec policy transactionally; config owns the resulting paths. */
int sandbox_prepare_tool_paths(struct sandbox_config *cfg);

#ifdef __cplusplus
}
#endif
#endif
