#ifndef MORPH_RUNTIME_MCP_H
#define MORPH_RUNTIME_MCP_H

#include "config.h"
#include "mcp/mcp.h"

int runtime_mcp_server_config_from_config(const struct config_mcp_server *config,
					  struct mcp_server_config *out);
int runtime_mcp_register_configured(struct mcp_registry *registry,
				    const struct config_mcp *config);
int runtime_mcp_init_from_config(struct mcp_registry *registry,
				 const struct config_mcp *config,
				 struct tool_registry *tools,
				 int connect_configured);

#endif
