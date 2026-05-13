#ifndef TEXT_QA_H
#define TEXT_QA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"
#include "models/llm.h"

int text_qa_init(struct tool_registry *reg, struct model *llm);

#ifdef __cplusplus
}
#endif

#endif
