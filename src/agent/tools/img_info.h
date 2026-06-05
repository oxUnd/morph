#ifndef IMG_INFO_H
#define IMG_INFO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"

struct tool_context;

int img_info_init(struct tool_registry *reg, struct tool_context *tctx);

#ifdef __cplusplus
}
#endif

#endif
