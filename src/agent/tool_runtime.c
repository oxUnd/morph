#include "tool_runtime.h"
#include <stddef.h>

static _Thread_local struct tool_runtime_context current_tool_runtime;
static _Thread_local int current_tool_runtime_set;

void tool_runtime_set_current(const struct tool_runtime_context *ctx)
{
	if (ctx) {
		current_tool_runtime = *ctx;
		current_tool_runtime_set = 1;
	} else {
		current_tool_runtime = (struct tool_runtime_context){0};
		current_tool_runtime_set = 0;
	}
}

const struct tool_runtime_context *tool_runtime_get_current(void)
{
	if (!current_tool_runtime_set)
		return NULL;
	return &current_tool_runtime;
}
