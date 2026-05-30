#ifndef FILE_INFO_H
#define FILE_INFO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"

struct tool_context;

int file_info_init(struct tool_registry *reg, struct tool_context *tctx);

#ifdef __cplusplus
}
#endif

#endif
