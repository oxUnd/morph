#ifndef IMG_EDIT_H
#define IMG_EDIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"
#include "models/llm.h"

struct tool_context;

int img_edit_init(struct tool_registry *reg, struct model *llm,
		  struct tool_context *tctx);

#ifdef __cplusplus
}
#endif

#endif
