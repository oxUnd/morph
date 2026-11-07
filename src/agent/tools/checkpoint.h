#ifndef MORPH_CHECKPOINT_TOOL_H
#define MORPH_CHECKPOINT_TOOL_H

#ifdef __cplusplus
extern "C" {
#endif

struct tool_registry;

int checkpoint_init(struct tool_registry *registry);

#ifdef __cplusplus
}
#endif

#endif
