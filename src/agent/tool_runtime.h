#ifndef TOOL_RUNTIME_H
#define TOOL_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

#include "config/config.h"
#include "db/database.h"
#include "event/event.h"
#include "util/buf.h"
#include "http/client.h"
#include "agent/tools/ask_user.h"
#include <stdint.h>

typedef int (*tool_runtime_session_visible_fn)(const char *display_id,
					       const char *user_id,
					       void *user_data);

struct tool_runtime_context {
	struct http_control http;
	ask_user_callback_fn ask_user_fn;
	void *ask_user_user_data;
	int sub_agent_depth;
	struct db *db;
	const struct config *config;
	const char *user_id;
	const char *credit_session_id;
	int64_t memory_session_id;
	int restrict_memory_to_user;
	tool_runtime_session_visible_fn memory_visible_fn;
	void *memory_visible_user_data;
	morph_event_cb event_cb;
	void *event_user_data;
	const char *turn_id;
	const char *tool_call_id;
	const char *tool_catalog_json;
};

struct tool_runtime_stream_sink {
	const struct tool_runtime_context *runtime;
	const char *tool;
	const char *kind;
	morph_buf_t *buf;
};

int tool_runtime_emit_stream(const struct tool_runtime_context *ctx, const char *tool,
			     const char *kind, const char *text);
int tool_runtime_stream_to_buf_cb(const char *token, void *user_data);

#ifdef __cplusplus
}
#endif

#endif
