#include "jsonrpc.h"
#include "cJSON.h"
#include "util/error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

char *jsonrpc_build_request(const struct jsonrpc_request *req)
{
	if (!req || !req->method)
		return NULL;

	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;

	cJSON_AddStringToObject(root, "jsonrpc", "2.0");
	cJSON_AddNumberToObject(root, "id", req->id);
	cJSON_AddStringToObject(root, "method", req->method);

	if (req->params_json && *req->params_json) {
		cJSON *params = cJSON_Parse(req->params_json);
		if (params) {
			cJSON_AddItemToObject(root, "params", params);
		} else {
			cJSON_AddStringToObject(root, "params", req->params_json);
		}
	}

	char *str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return str;
}

int jsonrpc_parse_response(const char *resp_str, struct jsonrpc_response *out)
{
	if (!resp_str || !out)
		MORPH_RETURN(-EINVAL);

	memset(out, 0, sizeof(*out));

	cJSON *root = cJSON_Parse(resp_str);
	if (!root)
		MORPH_RETURN(-EPROTO);

	cJSON *id_item = cJSON_GetObjectItem(root, "id");
	if (cJSON_IsNumber(id_item))
		out->id = id_item->valueint;

	cJSON *error_item = cJSON_GetObjectItem(root, "error");
	if (cJSON_IsObject(error_item)) {
		out->has_error = 1;
		cJSON *code = cJSON_GetObjectItem(error_item, "code");
		if (cJSON_IsNumber(code))
			out->error_code = code->valueint;
		cJSON *msg = cJSON_GetObjectItem(error_item, "message");
		if (cJSON_IsString(msg) && msg->valuestring)
			out->error_message = strdup(msg->valuestring);
		cJSON_Delete(root);
		return 0;
	}

	cJSON *result_item = cJSON_GetObjectItem(root, "result");
	if (result_item) {
		char *result_str = cJSON_PrintUnformatted(result_item);
		out->result_json = result_str;
	} else {
		out->has_error = 1;
		out->error_code = -32603;
		out->error_message = strdup("response missing result");
	}

	cJSON_Delete(root);
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
