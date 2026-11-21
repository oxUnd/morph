#ifndef MORPH_RUNTIME_PROTOCOL_H
#define MORPH_RUNTIME_PROTOCOL_H

#include "cJSON.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RUNTIME_PROTOCOL_VERSION 1

struct runtime_command {
	const char *id;
	const char *method;
	const cJSON *params;
};

struct runtime_reply {
	const char *id;
	const cJSON *result;
	int error_code;
};

enum runtime_message_kind {
	RUNTIME_MESSAGE_RESPONSE,
	RUNTIME_MESSAGE_EVENT,
	RUNTIME_MESSAGE_SERVER_REQUEST,
};

/* Each message owns all pointers, including data. Event data is the original
 * morph_event JSON envelope; synthetic lifecycle events use the same envelope. */
struct runtime_message {
	enum runtime_message_kind kind;
	char *id;
	char *method;
	int64_t session_id;
	char *turn_id;
	uint64_t sequence;
	cJSON *data;
	int error_code;
};

void runtime_message_destroy(struct runtime_message *message);
void runtime_message_cleanup(struct runtime_message **message);

#ifdef __cplusplus
}
#endif
#endif
