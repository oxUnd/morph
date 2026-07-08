#ifndef CONFIG_WRITE_H
#define CONFIG_WRITE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"

struct tool_context;

int config_write_init(struct tool_registry *reg, struct tool_context *tctx,
		      const char *config_path);

#ifdef __cplusplus
}
#endif

#endif
