#ifndef MORPH_DYNAMIC_TOOL_RUNNER_H
#define MORPH_DYNAMIC_TOOL_RUNNER_H

#include "dynamic_tool_internal.h"

void dynamic_tool_runner_destroy(void *opaque);
int dynamic_tool_runner_cancelled(void *opaque);
int dynamic_tool_runner_execute(const char *args_json, struct tool_result *result, void *user_data);
/* Optional diagnostics are malloc owners, populated on validation failure.
 * Supplied outputs must be NULL; occupied outputs are preserved. */
int dynamic_tool_runner_check_js_source(const char *source_path, char **error_out);
int dynamic_tool_runner_check_unpublished_source(const char *source, char **error_out);

#endif
