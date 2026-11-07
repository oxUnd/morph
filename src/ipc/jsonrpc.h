#ifndef JSONRPC_H
#define JSONRPC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

struct jsonrpc_request {
	int id;
	const char *method;
	const char *params_json;
};

struct jsonrpc_response {
	int id;
	char *result_json;
	int has_error;
	int error_code;
	char *error_message;
};

/* Independent serialized JSON; release with cJSON_free(). */
char *jsonrpc_request_create(const struct jsonrpc_request *req);
/* Borrows input only during parsing; out must have no owned fields.
 * Success owns independent fields; failure preserves out. Release using
 * jsonrpc_response_free(), including partially constructed valid owners. */
int jsonrpc_response_copy(const char *resp_str, struct jsonrpc_response *out);
void jsonrpc_response_free(struct jsonrpc_response *resp);

#ifdef __cplusplus
}
#endif

#endif
