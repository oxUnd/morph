#ifndef IMG_EDIT_H
#define IMG_EDIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"
#include "models/llm.h"

int img_edit_init(struct tool_registry *reg, struct model *llm);

#ifdef __cplusplus
}
#endif

#endif