#include "jsonrpc.h"
#include "cJSON.h"
#include "util/cleanup.h"
#include "util/error.h"
#include "util/json.h"
#include <stdlib.h>
#include <string.h>

char *jsonrpc_request_create(const struct jsonrpc_request *req)
{
	cJSON *root MORPH_CLEANUP(morph_json_cleanup) = NULL;
	cJSON *params MORPH_CLEANUP(morph_json_cleanup) = NULL;

	if (!req || !req->method)
		return NULL;
	root = cJSON_CreateObject();
	if (!root)
		return NULL;
	if (!cJSON_AddStringToObject(root, "jsonrpc", "2.0") ||
	    !cJSON_AddNumberToObject(root, "id", req->id) ||
	    !cJSON_AddStringToObject(root, "method", req->method))
		return NULL;
	if (req->params_json && *req->params_json) {
		params = cJSON_Parse(req->params_json);
		if (!params)
			params = cJSON_CreateString(req->params_json);
		if (!params || morph_json_object_take(root, "params", &params) < 0)
			return NULL;
	}
	return morph_json_print_copy(root);
}

int jsonrpc_response_copy(const char *resp_str, struct jsonrpc_response *out)
{
	struct jsonrpc_response candidate MORPH_CLEANUP(jsonrpc_response_free) = {0};
	cJSON *root MORPH_CLEANUP(morph_json_cleanup) = NULL;

	if (!resp_str || !out)
		return -EINVAL;
	if (out->result_json || out->error_message)
		return -EEXIST;
	root = cJSON_Parse(resp_str);
	if (!root)
		return -EPROTO;
	cJSON *id_item = cJSON_GetObjectItem(root, "id");

	if (cJSON_IsNumber(id_item))
		candidate.id = id_item->valueint;
	cJSON *error_item = cJSON_GetObjectItem(root, "error");

	if (cJSON_IsObject(error_item)) {
		candidate.has_error = 1;
		cJSON *code = cJSON_GetObjectItem(error_item, "code");
		cJSON *msg = cJSON_GetObjectItem(error_item, "message");

		if (cJSON_IsNumber(code))
			candidate.error_code = code->valueint;
		if (cJSON_IsString(msg) && msg->valuestring) {
			candidate.error_message = strdup(msg->valuestring);
			if (!candidate.error_message)
				return -ENOMEM;
		}
	} else {
		cJSON *result_item = cJSON_GetObjectItem(root, "result");

		if (result_item) {
			candidate.result_json = morph_json_print_copy(result_item);
			if (!candidate.result_json)
				return -ENOMEM;
		} else {
			candidate.has_error = 1;
			candidate.error_code = -32603;
			candidate.error_message = strdup("response missing result");
			if (!candidate.error_message)
				return -ENOMEM;
		}
	}
	*out = candidate;
	memset(&candidate, 0, sizeof(candidate));
	return 0;
}

void jsonrpc_response_free(struct jsonrpc_response *resp)
{
	if (!resp)
		return;
	if (resp->result_json)
		cJSON_free(resp->result_json);
	free(resp->error_message);
	memset(resp, 0, sizeof(*resp));
}
