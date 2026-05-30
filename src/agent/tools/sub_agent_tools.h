#ifndef SUB_AGENT_TOOLS_H
#define SUB_AGENT_TOOLS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"
#include "agent/sub_agent.h"

struct sub_agent_tool_binding {
	struct sub_agent_runtime *rt;
	char agent_name[SUB_AGENT_NAME_MAX];
};

int sub_agent_sync_init(struct tool_registry *reg,
			struct sub_agent_runtime *rt);
int sub_agent_delegate_init(struct tool_registry *reg,
			    struct sub_agent_runtime *rt);
int sub_agent_status_init(struct tool_registry *reg,
			  struct sub_agent_runtime *rt);
int sub_agent_fanout_init(struct tool_registry *reg,
			  struct sub_agent_runtime *rt);
void sub_agent_tools_register_all(struct tool_registry *reg,
				  struct sub_agent_runtime *rt);

#ifdef __cplusplus
}
#endif

#endif
