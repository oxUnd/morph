#ifndef MORPH_CLI_PAGES_APPROVAL_H
#define MORPH_CLI_PAGES_APPROVAL_H

#include "agent/react.h"
#include "agent/tool_context.h"

struct cli_application;

void cli_approval_render_tool(struct cli_application *ctx, const char *tool_name,
			      const char *tool_args);
void cli_approval_render_operation(struct cli_application *ctx, const struct tool_operation *op);
enum hitl_verdict cli_approval_tool_callback(const char *tool_name, const char *tool_args,
					     void *user_data);
enum tool_operation_verdict cli_approval_operation_callback(const struct tool_operation *op,
							    void *user_data);

#endif
