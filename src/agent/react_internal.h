#ifndef MORPH_REACT_INTERNAL_H
#define MORPH_REACT_INTERNAL_H

#include "react.h"
#include "react_events.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Instance-local platform seam. Returns a positive pthread error; a failed join
 * must leave the worker joinable so the owner can drain it. Borrowed through drain. */
/* Shared only by the ReAct coordinator and its private request/worker modules. */
struct react_event_sink react_events(const struct react_context *ctx);
void react_set_error_detail(struct react_context *ctx, const char *detail);
struct tool_handle *react_builtin_acquire(struct react_context *ctx, const char *name,
					  struct tool_handle *handle);
struct chat_message *react_push_chat_message(morph_array_t *messages);

struct react_thread_ops {
	int (*join)(pthread_t thread, void *user_data);
	void *user_data;
};

#ifdef __cplusplus
}
#endif
#endif
