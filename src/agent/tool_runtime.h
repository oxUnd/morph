#ifndef TOOL_RUNTIME_H
#define TOOL_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"
#include "db/database.h"
#include <stdint.h>

typedef int (*tool_runtime_session_visible_fn)(const char *display_id,
					       const char *user_id,
					       void *user_data);

struct tool_runtime_context {
	struct db *db;
	const struct config *config;
	const char *user_id;
	const char *credit_session_id;
	int64_t memory_session_id;
	int restrict_memory_to_user;
	tool_runtime_session_visible_fn memory_visible_fn;
	void *memory_visible_user_data;
};

void tool_runtime_set_current(const struct tool_runtime_context *ctx);
const struct tool_runtime_context *tool_runtime_get_current(void);

#ifdef __cplusplus
}
#endif

#endif
