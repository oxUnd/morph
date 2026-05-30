#ifndef VID_GEN_H
#define VID_GEN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"
#include "models/llm.h"

struct tool_context;

int vid_gen_init(struct tool_registry *reg, struct model *video_llm,
		 struct tool_context *tctx);

#ifdef __cplusplus
}
#endif

#endif
