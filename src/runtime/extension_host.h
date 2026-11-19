#ifndef MORPH_RUNTIME_EXTENSION_HOST_H
#define MORPH_RUNTIME_EXTENSION_HOST_H

struct tool_result;

/* Registry callback. user_data is the borrowed loaded extension. */
int runtime_extension_exec(const char *args_json, struct tool_result *result,
			   void *user_data);

#endif
