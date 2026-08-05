#ifndef AGENT_TOOLS_APPLY_PATCH_H
#define AGENT_TOOLS_APPLY_PATCH_H

#ifdef __cplusplus
extern "C" {
#endif

struct tool_context;
struct tool_registry;

int apply_patch_init(struct tool_registry *registry,
		     struct tool_context *tool_context);

#ifdef __cplusplus
}
#endif

#endif
