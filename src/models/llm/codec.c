#include "codec.h"
#include "chat_codec.h"
#include "responses_codec.h"
#include <nb/log.h>
#include "errors.h"
#include "models/llm/options.h"
#include "cJSON.h"
#include <nb/cleanup.h>
#include <nb/str.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int llm_message_contains_icase(const char *message, const char *phrase)
{
	nb_str text = nb_str_from_cstr(message ? message : "");
	nb_str needle = nb_str_from_cstr(phrase);

	if (text.len < needle.len)
		return 0;

	for (size_t i = 0; i <= text.len - needle.len; i++) {
		if (nb_str_eq_icase(nb_str_from_parts(text.ptr + i, needle.len), needle))
			return 1;
	}

	return 0;
}

int llm_codec_error_is_context(const char *code, const char *message)
{
	if (code && (!strcmp(code, "context_length_exceeded") ||
		     !strcmp(code, "context_window_exceeded") ||
		     !strcmp(code, "prompt_too_long") || !strcmp(code, "input_too_large")))
		return 1;

	return llm_message_contains_icase(message, "maximum context length") ||
	       llm_message_contains_icase(message, "exceeds the context window") ||
	       llm_message_contains_icase(message, "prompt is too long") ||
	       llm_message_contains_icase(message, "prompt too long") ||
	       llm_message_contains_icase(message, "input is too long") ||
	       llm_message_contains_icase(message, "too many input tokens") ||
	       llm_message_contains_icase(message, "input too large") ||
	       llm_message_contains_icase(message, "input is too large");
}

const cJSON *llm_codec_error_borrow(const cJSON *root)
{
	const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");

	if (error && !cJSON_IsNull(error))
		return error;

	const cJSON *response = cJSON_GetObjectItemCaseSensitive(root, "response");
	error = cJSON_GetObjectItemCaseSensitive(response, "error");
	return error && !cJSON_IsNull(error) ? error : root;
}

const char *llm_codec_error_code_borrow(const cJSON *error)
{
	const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "code"));

	if (!code) {
		const char *type =
			cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "type"));
		if (type && strcmp(type, "error"))
			code = type;
	}

	return code;
}

const char *llm_codec_error_message_borrow(const cJSON *error)
{
	if (cJSON_IsString(error))
		return cJSON_GetStringValue(error);

	return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "message"));
}

enum model_error_kind llm_codec_error_kind(const cJSON *error)
{
	const char *code = llm_codec_error_code_borrow(error);
	const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "type"));
	const char *message = llm_codec_error_message_borrow(error);

	if (llm_codec_error_is_context(code, message) || llm_codec_error_is_context(type, NULL))
		return MODEL_ERROR_CONTEXT;

	if (code && (!strcmp(code, "rate_limit_exceeded") || !strcmp(code, "rate_limit_error")))
		return MODEL_ERROR_RATE_LIMIT;

	if (code && (!strcmp(code, "server_error") || !strcmp(code, "internal_server_error") ||
		     !strcmp(code, "overloaded_error") || !strcmp(code, "service_unavailable")))
		return MODEL_ERROR_SERVER;

	if (code && (!strcmp(code, "invalid_api_key") || !strcmp(code, "authentication_error") ||
		     !strcmp(code, "permission_denied")))
		return MODEL_ERROR_AUTH;

	return MODEL_ERROR_API;
}

int llm_codec_stream_error(struct llm_stream_ctx *ctx, const cJSON *error,
			   const char *detail)
{
	const char *code = llm_codec_error_code_borrow(error);

	ctx->terminal_received = 1;
	ctx->terminal_kind = llm_codec_error_kind(error);
	if (llm_codec_error_is_context(NULL, detail))
		ctx->terminal_kind = MODEL_ERROR_CONTEXT;

	ctx->terminal_error = ctx->terminal_kind == MODEL_ERROR_CONTEXT
		? MORPH_ERR_CONTEXT_LIMIT : MORPH_ERR_API;
	llm_usage_copy(ctx->terminal_code, sizeof(ctx->terminal_code), code ? code : "");
	return llm_stream_string_copy(&ctx->terminal_detail,
				      detail ? detail : "Model stream returned an API error.");
}

const struct llm_codec *llm_codec_borrow(const struct model *model)
{
	return llm_adapter_borrow(model)->responses_api ? llm_responses_codec_borrow()
							: llm_chat_codec_borrow();
}

cJSON *llm_codec_schema_copy(const cJSON *params)
{
	cJSON *schema NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	if (cJSON_IsObject(params)) {
		schema = cJSON_Duplicate(params, 1);
		if (!schema)
			return NULL;
		if (!cJSON_HasObjectItem(schema, "type") &&
		    !cJSON_AddStringToObject(schema, "type", "object"))
			return NULL;
	} else if (cJSON_IsArray(params)) {
		schema = cJSON_CreateObject();
		cJSON *required NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateArray();
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
				cJSON *entry NB_CLEANUP(cJSON_DeleteOwner) =
					cJSON_CreateString(name->valuestring);
				if (!entry ||
				    (cJSON_AddItemToArray(required, entry) ? (entry = NULL, NB_OK)
									   : NB_ERR_NOMEM) != 0)
					return NULL;
			}
		}
		if (cJSON_GetArraySize(required) &&
		    (cJSON_AddItemToObject(schema, "required", required) ? (required = NULL, NB_OK)
									 : NB_ERR_NOMEM) != 0)
			return NULL;
	} else {
		return NULL;
	}
	cJSON *owned = schema;
	schema = NULL;
	return owned;
}

int llm_codec_add_options(struct nb_log *logger, cJSON *root, const char *json)
{
	cJSON *extra;
	cJSON *item;

	if (!json || !json[0])
		return 0;
	extra = cJSON_ParseWithOpts(json, NULL, 1);
	if (!extra || !cJSON_IsObject(extra)) {
		cJSON_Delete(extra);
		nb_log_write(logger, NB_LOG_ERROR,
			     "extra_body_json must contain a valid JSON object");
		return nb_err_trace(logger, MORPH_ERR_CONFIG, morph_error_descriptions(),
				    MORPH_ERROR_COUNT, __FILE__, __LINE__);
	}
	cJSON_ArrayForEach (item, extra) {
		cJSON *copy;

		if (!item->string || llm_option_key_is_reserved(item->string)) {
			nb_log_write(logger, NB_LOG_ERROR,
				     "extra_body_json contains reserved or invalid field: %s",
				     item->string ? item->string : "<unnamed>");
			cJSON_Delete(extra);
			return nb_err_trace(logger, MORPH_ERR_CONFIG, morph_error_descriptions(),
					    MORPH_ERROR_COUNT, __FILE__, __LINE__);
		}
		copy = cJSON_Duplicate(item, 1);
		if (!copy || !cJSON_AddItemToObject(root, item->string, copy)) {
			cJSON_Delete(copy);
			cJSON_Delete(extra);
			return nb_err_trace(logger, NB_ERR_NOMEM, morph_error_descriptions(),
					    MORPH_ERROR_COUNT, __FILE__, __LINE__);
		}
	}
	cJSON_Delete(extra);
	return 0;
}
