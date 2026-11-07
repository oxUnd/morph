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

struct json *llm_codec_schema(struct json *params)
{
	if (!params)
		return NULL;
	if (json_is_object_value(params)) {
		if (!json_has_member_ci(params, "type"))
			json_add_string(params, "type", "object");
		return params;
	}
	if (json_is_array_value(params)) {
		struct json *schema = json_object();
		json_add_string(schema, "type", "object");
		struct json *props = json_object();
		struct json *required = json_array();
		struct json *item;
		json_foreach (item, params) {
			struct json *name = json_member_ci(item, "name");
			if (!json_is_string_value(name))
				continue;
			struct json *prop = json_object();
			json_add_string(prop, "type", "string");
			struct json *desc = json_member_ci(item, "description");
			if (json_is_string_value(desc))
				json_add_string(prop, "description", json_string_value(desc));
			json_object_add(props, json_string_value(name), prop);
			struct json *req = json_member_ci(item, "required");
			if (json_is_bool_value(req) && json_is_true(req))
				json_array_add(required, json_string(json_string_value(name)));
		}
		json_object_add(schema, "properties", props);
		if (json_count(required) > 0)
			json_object_add(schema, "required", required);
		else
			json_free(required);
		json_free(params);
		return schema;
	}
	json_free(params);
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

int llm_codec_add_options(struct morph_log *logger, struct json *root, const char *json)
{
	struct json *extra;
	const struct json *item;

	if (!json || !json[0])
		return 0;
	extra = json_parse_ex(json, NULL, 1, NULL);
	if (!extra || !json_is_object_value(extra)) {
		json_free(extra);
		log_err(logger, "extra_body_json must contain a valid JSON object");
		return MORPH_TRACE_ERROR(logger, MORPH_ERR_CONFIG);
	}
	json_foreach (item, extra) {
		struct json *copy;
		const char *key = json_key(item);

		if (!key || extra_body_key_is_reserved(key)) {
			log_err(logger, "extra_body_json contains reserved or invalid field: %s",
				key ? key : "<unnamed>");
			json_free(extra);
			return MORPH_TRACE_ERROR(logger, MORPH_ERR_CONFIG);
		}
		copy = json_clone(item, 1);
		if (!copy || json_object_add(root, key, copy) != 0) {
			json_free(copy);
			json_free(extra);
			return MORPH_TRACE_ERROR(logger, -ENOMEM);
		}
	}
	json_free(extra);
	return 0;
}
