#ifndef VID_GEN_H
#define VID_GEN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"
#include "models/llm.h"

int vid_gen_init(struct tool_registry *reg, struct model *video_llm);

#ifdef __cplusplus
}
#endif

#endif
