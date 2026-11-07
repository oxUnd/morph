#include "jsonrpc.h"
#include "util/json.h"
#include "util/error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

char *jsonrpc_build_request(const struct jsonrpc_request *req)
{
	if (!req || !req->method)
		return NULL;

	struct json *root = json_object();
	if (!root)
		return NULL;

	if (!json_add_string(root, "jsonrpc", "2.0") || !json_add_number(root, "id", req->id) ||
	    !json_add_string(root, "method", req->method)) {
		json_free(root);
		return NULL;
	}

	if (req->params_json && *req->params_json) {
		struct json *params = json_parse_prefix(req->params_json, NULL, NULL);
		if (params) {
			if (json_object_add(root, "params", params) != 0) {
				json_free(params);
				json_free(root);
				return NULL;
			}
		} else {
			if (!json_add_string(root, "params", req->params_json)) {
				json_free(root);
				return NULL;
			}
		}
	}

	char *str = json_stringify(root);
	json_free(root);
	return str;
}

int jsonrpc_parse_response(const char *resp_str, struct jsonrpc_response *out)
{
	if (out == NULL) {
		return -EINVAL;
	}
	memset(out, 0, sizeof(*out));
	if (resp_str == NULL) {
		return -EINVAL;
	}

	struct json *root = json_parse_prefix(resp_str, NULL, NULL);
	if (!root)
		return -EPROTO;

	const struct json *id_item = json_member_ci(root, "id");
	if (json_is_number_value(id_item))
		out->id = json_int_value(id_item);

	const struct json *error_item = json_member_ci(root, "error");
	if (json_is_object_value(error_item)) {
		out->has_error = 1;
		const struct json *code = json_member_ci(error_item, "code");
		if (json_is_number_value(code))
			out->error_code = json_int_value(code);
		const char *message = json_string_value(json_member_ci(error_item, "message"));
		if (message) {
			out->error_message = strdup(message);
			if (out->error_message == NULL) {
				json_free(root);
				return -ENOMEM;
			}
		}
		json_free(root);
		return 0;
	}

	const struct json *result_item = json_member_ci(root, "result");
	if (result_item) {
		char *result_str = json_stringify(result_item);
		if (result_str == NULL) {
			json_free(root);
			return -ENOMEM;
		}
		out->result_json = result_str;
	} else {
		out->has_error = 1;
		out->error_code = -32603;
		out->error_message = strdup("response missing result");
		if (out->error_message == NULL) {
			json_free(root);
			return -ENOMEM;
		}
	}

	json_free(root);
	return 0;
}

void jsonrpc_response_free(struct jsonrpc_response *resp)
{
	if (!resp)
		return;
	free(resp->result_json);
	free(resp->error_message);
	memset(resp, 0, sizeof(*resp));
}
