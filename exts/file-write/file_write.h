#ifndef MORPH_FILE_WRITE_EXT_H
#define MORPH_FILE_WRITE_EXT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"

struct tool_context;

int file_write_tool_init(struct tool_registry *reg, struct tool_context *tctx);

#ifdef __cplusplus
}
#endif

#endif /* MORPH_FILE_WRITE_EXT_H */
