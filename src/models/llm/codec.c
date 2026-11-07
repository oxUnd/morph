#include "codec.h"
#include "chat_codec.h"
#include "responses_codec.h"
#include "util/log.h"
#include "util/error.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

const struct llm_codec *llm_codec_resolve(const struct model *model)
{
	return llm_adapter_resolve(model)->responses_api ? llm_responses_codec() : llm_chat_codec();
}

cJSON *llm_codec_schema(cJSON *params)
{
	if (!params)
		return NULL;
	if (cJSON_IsObject(params)) {
		if (!cJSON_HasObjectItem(params, "type"))
			cJSON_AddStringToObject(params, "type", "object");
		return params;
	}
	if (cJSON_IsArray(params)) {
		cJSON *schema = cJSON_CreateObject();
		cJSON_AddStringToObject(schema, "type", "object");
		cJSON *props = cJSON_CreateObject();
		cJSON *required = cJSON_CreateArray();
		cJSON *item;
		cJSON_ArrayForEach (item, params) {
			cJSON *name = cJSON_GetObjectItem(item, "name");
			if (!cJSON_IsString(name))
				continue;
			cJSON *prop = cJSON_CreateObject();
			cJSON_AddStringToObject(prop, "type", "string");
			cJSON *desc = cJSON_GetObjectItem(item, "description");
			if (cJSON_IsString(desc))
				cJSON_AddStringToObject(prop, "description", desc->valuestring);
			cJSON_AddItemToObject(props, name->valuestring, prop);
			cJSON *req = cJSON_GetObjectItem(item, "required");
			if (cJSON_IsBool(req) && cJSON_IsTrue(req))
				cJSON_AddItemToArray(required,
						     cJSON_CreateString(name->valuestring));
		}
		cJSON_AddItemToObject(schema, "properties", props);
		if (cJSON_GetArraySize(required) > 0)
			cJSON_AddItemToObject(schema, "required", required);
		else
			cJSON_Delete(required);
		cJSON_Delete(params);
		return schema;
	}
	cJSON_Delete(params);
	return NULL;
}

static int extra_body_key_is_reserved(const char *key)
{
	static const char *const reserved[] = {"model",
					       "messages",
					       "tools",
					       "tool_choice",
					       "stream",
					       "stream_options",
					       "max_tokens",
					       "input",
					       "instructions",
					       "parallel_tool_calls",
					       "max_output_tokens"};

	for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
		if (strcmp(key, reserved[i]) == 0)
			return 1;
	}
	return 0;
}

int llm_codec_add_options(struct morph_log *logger, cJSON *root, const char *json)
{
	cJSON *extra;
	cJSON *item;

	if (!json || !json[0])
		return 0;
	extra = cJSON_ParseWithOpts(json, NULL, 1);
	if (!extra || !cJSON_IsObject(extra)) {
		cJSON_Delete(extra);
		log_err(logger, "extra_body_json must contain a valid JSON object");
		return MORPH_TRACE_ERROR(logger, MORPH_ERR_CONFIG);
	}
	cJSON_ArrayForEach (item, extra) {
		cJSON *copy;

		if (!item->string || extra_body_key_is_reserved(item->string)) {
			log_err(logger, "extra_body_json contains reserved or invalid field: %s",
				item->string ? item->string : "<unnamed>");
			cJSON_Delete(extra);
			return MORPH_TRACE_ERROR(logger, MORPH_ERR_CONFIG);
		}
		copy = cJSON_Duplicate(item, 1);
		if (!copy || !cJSON_AddItemToObject(root, item->string, copy)) {
			cJSON_Delete(copy);
			cJSON_Delete(extra);
			return MORPH_TRACE_ERROR(logger, -ENOMEM);
		}
	}
	cJSON_Delete(extra);
	return 0;
}
