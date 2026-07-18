#include "runtime/runtime_internal.h"

#include <errno.h>

int runtime_register_tool(struct runtime *runtime, const struct tool_spec *spec)
{
	int rc;

	if (!runtime || !spec)
		return -EINVAL;
	rc = tool_register(&runtime->context.tools, spec);
	if (rc == 0)
		(void)runtime_context_update_tool_runtime_context(&runtime->context,
			runtime->context.current_session.id);
	return rc;
}

int runtime_add_mcp_server(struct runtime *runtime,
			   const struct mcp_server_config *server)
{
	if (!runtime || !server)
		return -EINVAL;
	return mcp_registry_add(&runtime->context.mcp, server);
}
