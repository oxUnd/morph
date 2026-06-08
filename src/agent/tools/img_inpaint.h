#ifndef IMG_INPAINT_H
#define IMG_INPAINT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"
#include "models/llm.h"

struct tool_context;

int img_inpaint_init(struct tool_registry *reg, struct model *image_llm,
		     struct tool_context *tctx);

#ifdef __cplusplus
}
#endif

#endif
