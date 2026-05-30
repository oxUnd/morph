#ifndef FILE_READ_H
#define FILE_READ_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"

struct tool_context;

int file_read_init(struct tool_registry *reg, struct tool_context *tctx);

#ifdef __cplusplus
}
#endif

#endif
