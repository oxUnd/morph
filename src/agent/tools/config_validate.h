#ifndef CONFIG_VALIDATE_H
#define CONFIG_VALIDATE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"

struct tool_context;

int config_validate_init(struct tool_registry *reg, struct tool_context *tctx,
		     const char *config_path);

#ifdef __cplusplus
}
#endif

#endif
