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

char *jsonrpc_build_request(const struct jsonrpc_request *req);
int jsonrpc_parse_response(const char *resp_str, struct jsonrpc_response *out);
void jsonrpc_response_free(struct jsonrpc_response *resp);

#ifdef __cplusplus
}
#endif

#endif
