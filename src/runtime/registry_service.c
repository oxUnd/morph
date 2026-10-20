#include "runtime/runtime_internal.h"

#include <errno.h>

int runtime_register_tool(struct runtime *runtime, const struct tool_spec *spec)
{
	int rc;

	if (!runtime || !spec)
		return -EINVAL;
	pthread_mutex_lock(&runtime->context.execution_lock);
	pthread_mutex_lock(&runtime->context.tool_registry_lock);
	rc = tool_register(&runtime->context.tools, spec);
	if (rc == 0)
		(void)runtime_context_update_tool_runtime_context(&runtime->context,
			runtime->context.current_session.id);
	pthread_mutex_unlock(&runtime->context.tool_registry_lock);
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return rc;
}

int runtime_add_mcp_server(struct runtime *runtime,
			   const struct mcp_server_config *server)
{
	if (!runtime || !server)
		return -EINVAL;
	return mcp_registry_add(&runtime->context.mcp, server);
}
