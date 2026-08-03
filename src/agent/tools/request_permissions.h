#ifndef REQUEST_PERMISSIONS_H
#define REQUEST_PERMISSIONS_H

#ifdef __cplusplus
extern "C" {
#endif

struct tool_context;
struct tool_registry;

int request_permissions_init(struct tool_registry *reg,
			     struct tool_context *tctx);

#ifdef __cplusplus
}
#endif

#endif
