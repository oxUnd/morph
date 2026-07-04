#ifndef DYNAMIC_TOOLS_H
#define DYNAMIC_TOOLS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"
#include "agent/tool_context.h"
#include "config.h"

int dynamic_tools_init(struct tool_registry *reg, struct tool_context *tctx,
		       const struct config_dynamic_tools *cfg,
		       const char *session_id);
int dynamic_tools_set_session_id(struct tool_registry *reg,
				 const char *session_id);
int dynamic_tools_load_persistent(struct tool_registry *reg,
				  struct tool_context *tctx,
				  const struct config_dynamic_tools *cfg);

#ifdef __cplusplus
}
#endif

#endif
