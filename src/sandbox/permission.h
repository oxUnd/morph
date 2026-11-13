#ifndef MORPH_SANDBOX_PERMISSION_H
#define MORPH_SANDBOX_PERMISSION_H

#include "sandbox.h"
#include "nb/cleanup.h"

struct tool_context;
int sandbox_permission_prepare(struct tool_context *context, const char *principal,
			       const char *scratch, struct sandbox_config *out);
int sandbox_permission_intersect(struct sandbox_config *requested,
				 const struct sandbox_config *ceiling);

#endif
