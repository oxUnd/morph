#include "runtime/runtime_internal.h"
#include "nb/err.h"

#include <errno.h>

static int runtime_register_tool_spec(struct runtime *runtime, const struct tool_spec *spec,
				      struct tool_spec *owner)
{
	int rc;

	if (!runtime || !spec)
		return NB_ERR_INVAL;
	nb_mutex_lock(runtime->context.execution_lock);
	nb_mutex_lock(runtime->context.tools.mutation_mutex);
	nb_mutex_lock(runtime->context.tool_registry_lock);
	rc = owner ? tool_register_take(&runtime->context.tools, owner)
		   : tool_register_borrow(&runtime->context.tools, spec);
	if (rc == 0)
		(void)runtime_context_update_tool_runtime_context(
			&runtime->context, runtime->context.current_session.id);
	nb_mutex_unlock(runtime->context.tool_registry_lock);
	nb_mutex_unlock(runtime->context.tools.mutation_mutex);
	nb_mutex_unlock(runtime->context.execution_lock);
	return rc;
}

int runtime_register_tool_borrow(struct runtime *runtime, const struct tool_spec *spec)
{
	return runtime_register_tool_spec(runtime, spec, NULL);
}

int runtime_register_tool_take(struct runtime *runtime, struct tool_spec *spec)
{
	return runtime_register_tool_spec(runtime, spec, spec);
}

int runtime_add_mcp_server(struct runtime *runtime,
			   const struct mcp_server_config *server)
{
	if (!runtime || !server)
		return NB_ERR_INVAL;
	return mcp_registry_add(&runtime->context.mcp, server);
}
