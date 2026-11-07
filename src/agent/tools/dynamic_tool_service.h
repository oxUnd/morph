#ifndef MORPH_DYNAMIC_TOOL_SERVICE_H
#define MORPH_DYNAMIC_TOOL_SERVICE_H

#include "dynamic_tool_internal.h"

enum dynamic_tool_action {
	DYNAMIC_TOOL_CREATE,
	DYNAMIC_TOOL_PROMOTE,
	DYNAMIC_TOOL_DELETE,
	DYNAMIC_TOOL_HISTORY,
	DYNAMIC_TOOL_DIFF,
	DYNAMIC_TOOL_ROLLBACK,
};

/* Owns the registry mutation lock for the whole management transaction. */
int dynamic_tool_service_execute(struct dynamic_tools_context *context,
				 enum dynamic_tool_action action, const char *args,
				 struct tool_result *result);
int dynamic_tool_service_load_root(struct tool_registry *reg, struct tool_context *tctx,
				   const struct config_dynamic_tools *cfg, const char *root,
				   enum tool_origin origin);
int dynamic_tool_service_load_session(struct tool_registry *reg, struct tool_context *tctx,
				      const struct config_dynamic_tools *cfg,
				      const char *session_id);

#endif
