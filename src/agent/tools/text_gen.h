#ifndef TEXT_GEN_H
#define TEXT_GEN_H

#include "agent/tool.h"
#include "models/llm.h"

int text_gen_init(struct tool_registry *reg, struct model *llm);

#endif