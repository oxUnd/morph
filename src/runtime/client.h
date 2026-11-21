#ifndef MORPH_RUNTIME_CLIENT_H
#define MORPH_RUNTIME_CLIENT_H

#include "runtime/protocol.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct runtime;
struct runtime_client;

enum runtime_client_capability {
	RUNTIME_CLIENT_READ = 1u << 0,
	RUNTIME_CLIENT_EXECUTE = 1u << 1,
	RUNTIME_CLIENT_INTERACT = 1u << 2,
	RUNTIME_CLIENT_MANAGE = 1u << 3,
	RUNTIME_CLIENT_ALL = 15u,
};

struct runtime_client_options {
	/* Trusted host identity, copied on open. NULL uses the local identity. */
	const char *user_id;
	/* Zero uses all local capabilities for compatibility with embedded hosts. */
	unsigned capabilities;
	size_t request_capacity;
	size_t event_capacity;
	/* Only signal an event loop. Must not call client/runtime APIs synchronously.
	 * Callback data remains borrowed until runtime_client_close returns. */
	void (*wakeup)(void *user_data);
	void *wakeup_user_data;
	/* Explicit host capability: this client can hand its terminal to an editor. */
	int terminal_handoff;
};

int runtime_client_open(struct runtime *runtime,
	const struct runtime_client_options *options, struct runtime_client **out);
/* Copy all inputs before returning. Queue rejection leaves caller data intact.
 * Success accepts the request; its response arrives on the receive channel. */
int runtime_client_request_copy(struct runtime_client *client,
	const struct runtime_command *command);
/* Replies resolve server requests independently of normal request capacity. */
int runtime_client_reply_copy(struct runtime_client *client,
	const struct runtime_reply *reply);
/* Empty owned output required. AGAIN means no message, CLOSED means shutdown.
 * receive waits at most timeout_ms; a negative timeout waits until a message. */
int runtime_client_try_receive(struct runtime_client *client,
	struct runtime_message **out);
int runtime_client_receive(struct runtime_client *client, int timeout_ms,
	struct runtime_message **out);
/* Disconnects this client only. Accepted turns continue and remain queryable. */
void runtime_client_close(struct runtime_client *client);

#ifdef __cplusplus
}
#endif
#endif
