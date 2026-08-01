#ifndef MORPH_RUNTIME_MCP_H
#define MORPH_RUNTIME_MCP_H

#include "config/config.h"
#include "event/event.h"
#include "mcp/mcp.h"

int runtime_mcp_server_config_from_config(const struct config_mcp_server *config,
					  struct mcp_server_config *out);
int runtime_mcp_register_configured(struct mcp_registry *registry,
				    const struct config_mcp *config);
int runtime_mcp_client_connect(struct mcp_client *client,
			       morph_event_cb event_cb,
			       void *event_user_data);
int runtime_mcp_client_disconnect(struct mcp_client *client,
				  morph_event_cb event_cb,
				  void *event_user_data);
int runtime_mcp_client_discover(struct mcp_client *client,
				struct tool_registry *tools,
				morph_event_cb event_cb,
				void *event_user_data,
				int *tools_count,
				int *resources_count,
				int *prompts_count);
int runtime_mcp_init_from_config(struct mcp_registry *registry,
				 const struct config_mcp *config,
				 struct tool_registry *tools,
				 int connect_configured,
				 morph_event_cb event_cb,
				 void *event_user_data);

#endif
