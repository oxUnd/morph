#include "codec.h"
#include "chat_codec.h"
#include "responses_codec.h"
#include "util/log.h"
#include "util/error.h"
#include "util/json.h"
#include "util/cleanup.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

const struct llm_codec *llm_codec_borrow(const struct model *model)
{
	return llm_adapter_borrow(model)->responses_api ? llm_responses_codec_borrow()
							: llm_chat_codec_borrow();
}

cJSON *llm_codec_schema_copy(const cJSON *params)
{
	cJSON *schema MORPH_CLEANUP(morph_json_cleanup) = NULL;
	if (cJSON_IsObject(params)) {
		schema = cJSON_Duplicate(params, 1);
		if (!schema)
			return NULL;
		if (!cJSON_HasObjectItem(schema, "type") &&
		    !cJSON_AddStringToObject(schema, "type", "object"))
			return NULL;
	} else if (cJSON_IsArray(params)) {
		schema = cJSON_CreateObject();
		cJSON *required MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateArray();
		cJSON *properties = schema ? cJSON_AddObjectToObject(schema, "properties") : NULL;
		if (!schema || !required || !properties ||
		    !cJSON_AddStringToObject(schema, "type", "object"))
			return NULL;
		const cJSON *item;
		cJSON_ArrayForEach (item, params) {
			const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
			if (!cJSON_IsString(name) || !name->valuestring)
				continue;
			cJSON *property = cJSON_AddObjectToObject(properties, name->valuestring);
			if (!property || !cJSON_AddStringToObject(property, "type", "string"))
				return NULL;
			const cJSON *description =
				cJSON_GetObjectItemCaseSensitive(item, "description");
			if (cJSON_IsString(description) &&
			    !cJSON_AddStringToObject(property, "description",
						     description->valuestring))
				return NULL;
			if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "required"))) {
				cJSON *entry MORPH_CLEANUP(morph_json_cleanup) =
					cJSON_CreateString(name->valuestring);
				if (!entry || morph_json_array_take(required, &entry) != 0)
					return NULL;
			}
		}
		if (cJSON_GetArraySize(required) &&
		    morph_json_object_take(schema, "required", &required) != 0)
			return NULL;
	} else {
		return NULL;
	}
	cJSON *owned = schema;
	schema = NULL;
	return owned;
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
