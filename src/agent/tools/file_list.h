#ifndef FILE_LIST_H
#define FILE_LIST_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"

struct tool_context;

int file_list_init(struct tool_registry *reg, struct tool_context *tctx);

#ifdef __cplusplus
}
#endif

#endif
