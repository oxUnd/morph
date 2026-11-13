#include "jsonrpc.h"
#include "cJSON.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

char *jsonrpc_request_create(const struct jsonrpc_request *req)
{
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = NULL;

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
		if (!params ||
		    (cJSON_AddItemToObject(root, "params", params) ? (params = NULL, NB_OK)
								   : NB_ERR_NOMEM) < 0)
			return NULL;
	}
	return cJSON_PrintUnformatted(root);
}

int jsonrpc_response_copy(const char *resp_str, struct jsonrpc_response *out)
{
	struct jsonrpc_response candidate NB_CLEANUP(jsonrpc_response_free) = {0};
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!resp_str || !out)
		return NB_ERR_INVAL;
	if (out->result_json || out->error_message)
		return NB_ERR_EXISTS;
	root = cJSON_Parse(resp_str);
	if (!root)
		return NB_ERR_PROTOCOL;
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
				return NB_ERR_NOMEM;
		}
	} else {
		cJSON *result_item = cJSON_GetObjectItem(root, "result");

		if (result_item) {
			candidate.result_json = cJSON_PrintUnformatted(result_item);
			if (!candidate.result_json)
				return NB_ERR_NOMEM;
		} else {
			candidate.has_error = 1;
			candidate.error_code = -32603;
			candidate.error_message = strdup("response missing result");
			if (!candidate.error_message)
				return NB_ERR_NOMEM;
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
