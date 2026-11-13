#ifndef MORPH_REACT_INTERNAL_H
#define MORPH_REACT_INTERNAL_H

#include "react.h"
#include "react_events.h"
#include "nb/thread.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Instance-local platform seam. Returns a native nbase status; a failed join
 * must leave the worker joinable so the owner can drain it. Borrowed through drain. */
/* Shared only by the ReAct coordinator and its private request/worker modules. */
struct react_event_sink react_events_borrow(const struct react_context *ctx);
void react_set_error_detail_copy(struct react_context *ctx, const char *detail);
struct tool_handle *react_builtin_acquire(struct react_context *ctx, const char *name,
					  struct tool_handle *handle);
struct chat_message *react_chat_message_push_borrow(nb_vec *messages);

struct react_thread_ops {
	int (*join)(nb_thread *thread, void *user_data);
	void *user_data;
};

#ifdef __cplusplus
}
#endif
#endif
