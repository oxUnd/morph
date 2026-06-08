#ifndef IMG_COMPOSE_H
#define IMG_COMPOSE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"
#include "models/llm.h"

struct tool_context;

int img_compose_init(struct tool_registry *reg, struct model *image_llm,
		     struct tool_context *tctx);

#ifdef __cplusplus
}
#endif

#endif
