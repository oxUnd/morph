#ifndef TEXT_GEN_H
#define TEXT_GEN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"
#include "models/llm.h"

int text_gen_init(struct tool_registry *reg, struct model *llm);

#ifdef __cplusplus
}
#endif

#endif