#include "js_fetch.h"
#include "runner_api.h"
#include "http/client.h"
#include "errors.h"
#include "nb/vec.h"
#include "nb/cleanup.h"
#include "nb/buf.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct fetch_headers {
	JSContext *ctx;
	JSValue object;
	JSPropertyEnum *properties;
	uint32_t count;
	nb_vec lines;
	size_t bytes;
};

static void fetch_headers_cleanup(struct fetch_headers *headers)
{
	for (size_t i = 0; i < headers->lines.len; i++)
		free(*(char **)nb_vec_at(&headers->lines, i));
	nb_vec_cleanup(&headers->lines);
	JS_FreePropertyEnum(headers->ctx, headers->properties, headers->count);
	JS_FreeValue(headers->ctx, headers->object);
}

struct fetch_header_pair {
	JSContext *ctx;
	JSValue name;
	JSValue value;
	const char *name_text;
	const char *value_text;
};

static void fetch_header_pair_cleanup(struct fetch_header_pair *pair)
{
	JS_FreeCString(pair->ctx, pair->name_text);
	JS_FreeCString(pair->ctx, pair->value_text);
	JS_FreeValue(pair->ctx, pair->name);
	JS_FreeValue(pair->ctx, pair->value);
}

static int fetch_header_append(struct fetch_headers *headers, JSAtom atom)
{
	JSContext *ctx = headers->ctx;
	struct fetch_header_pair pair NB_CLEANUP(fetch_header_pair_cleanup) = {
		.ctx = ctx,
		.name = JS_UNDEFINED,
		.value = JS_UNDEFINED,
	};
	nb_buf line NB_CLEANUP(nb_buf_cleanup) = {0};
	size_t name_len;
	size_t value_len;
	size_t limit = runner_capture_limit(ctx);
	int rc;

	pair.name = JS_AtomToValue(ctx, atom);
	pair.value = JS_GetProperty(ctx, headers->object, atom);
	if (JS_IsException(pair.name) || JS_IsException(pair.value))
		return NB_ERR_INVAL;
	pair.name_text = JS_ToCStringLen(ctx, &name_len, pair.name);
	pair.value_text = JS_ToCStringLen(ctx, &value_len, pair.value);
	if (!pair.name_text || !pair.value_text)
		return NB_ERR_INVAL;
	int valid = name_len > 0;
	for (size_t i = 0; valid && i < name_len; i++) {
		unsigned char c = (unsigned char)pair.name_text[i];
		valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c));
	}
	for (size_t i = 0; valid && i < value_len; i++) {
		unsigned char c = (unsigned char)pair.value_text[i];
		valid = (c >= 0x20 && c != 0x7f) || c == '\t';
	}
	if (!valid) {
		JS_ThrowTypeError(ctx, "fetch headers contain an invalid name or "
				 "control character");
		return NB_ERR_INVAL;
	}
	if (name_len > limit || value_len > limit - name_len ||
	    name_len + value_len > limit - (limit >= 2 ? 2 : limit)) {
		JS_ThrowRangeError(ctx, "fetch headers exceed the configured capture limit");
		return NB_ERR_TOO_BIG;
	}
	rc = (nb_buf_printf(&line, "%s: %s", pair.name_text, pair.value_text) ? 0 : NB_ERR_NOMEM);
	if (rc == 0 && line.len > limit - headers->bytes)
		rc = NB_ERR_TOO_BIG;
	if (rc == 0) {
		char **slot = nb_vec_push(&headers->lines);
		if (!slot)
			rc = NB_ERR_NOMEM;
		else {
			headers->bytes += line.len;
			*slot = (nb_buf_ok(&line) ? strdup(nb_buf_cstr(&line)) : NULL);
			if (!*slot)
				rc = NB_ERR_NOMEM;
		}
	}
	if (rc != 0)
		JS_ThrowInternalError(
			ctx, "fetch headers failed: %s",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	return rc;
}

static int fetch_headers_create(struct fetch_headers *headers, JSValueConst options)
{
	JSContext *ctx = headers->ctx;
	char **line_slot = NULL;
	int rc;

	if (JS_IsUndefined(options) || JS_IsNull(options))
		return 0;
	if (!JS_IsObject(options)) {
		JS_ThrowTypeError(ctx, "fetch options must be an object");
		return NB_ERR_INVAL;
	}
	headers->object = JS_GetPropertyStr(ctx, options, "headers");
	if (JS_IsException(headers->object))
		return NB_ERR_INVAL;
	if (JS_IsUndefined(headers->object) || JS_IsNull(headers->object))
		return 0;
	if (!JS_IsObject(headers->object)) {
		JS_ThrowTypeError(ctx, "fetch headers must be an object");
		return NB_ERR_INVAL;
	}
	if (JS_GetOwnPropertyNames(ctx, &headers->properties, &headers->count, headers->object,
				 JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0)
		return NB_ERR_INVAL;
	if (headers->count > INT_MAX) {
		JS_ThrowRangeError(ctx, "fetch has too many request headers");
		return NB_ERR_TOO_BIG;
	}
	rc = (nb_vec_init(&headers->lines, sizeof(*line_slot)),
	      nb_vec_reserve(&headers->lines, headers->count) ? 0 : NB_ERR_NOMEM);
	if (rc != 0) {
		JS_ThrowOutOfMemory(ctx);
		return rc;
	}
	for (uint32_t i = 0; i < headers->count; i++) {
		rc = fetch_header_append(headers, headers->properties[i].atom);
		if (rc != 0)
			return rc;
	}
	return 0;
}

static JSValue js_fetch_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	JSValue body;

	(void)argc;
	(void)argv;
	body = JS_GetPropertyStr(ctx, this_val, "_body");
	if (JS_IsException(body))
		return body;
	return body;
}

static JSValue js_fetch_to_string(JSContext *ctx, JSValueConst this_val, int argc,
				  JSValueConst *argv)
{
	return js_fetch_text(ctx, this_val, argc, argv);
}

static JSValue fetch_string_method(JSContext *ctx, JSValueConst response, const char *name,
				   int argc, JSValueConst *argv)
{
	JSValue body = JS_GetPropertyStr(ctx, response, "_body");
	JSValue method;
	JSValue result;

	if (JS_IsException(body))
		return body;
	method = JS_GetPropertyStr(ctx, body, name);
	result = JS_IsException(method) ? JS_EXCEPTION : JS_Call(ctx, method, body, argc, argv);
	JS_FreeValue(ctx, method);
	JS_FreeValue(ctx, body);
	return result;
}

static JSValue js_fetch_index_of(JSContext *ctx, JSValueConst this_val, int argc,
				 JSValueConst *argv)
{
	return fetch_string_method(ctx, this_val, "indexOf", argc, argv);
}

static JSValue js_fetch_slice(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	return fetch_string_method(ctx, this_val, "slice", argc, argv);
}

static JSValue js_fetch_substring(JSContext *ctx, JSValueConst this_val, int argc,
				  JSValueConst *argv)
{
	return fetch_string_method(ctx, this_val, "substring", argc, argv);
}

static JSValue make_fetch_response(JSContext *ctx, const char *url, const char *body, size_t len,
				   int status)
{
	JSValue object = JS_NewObject(ctx);
	JSValue text;
	int failed;

	if (JS_IsException(object))
		return object;
	text = JS_NewStringLen(ctx, body, len);
	if (JS_IsException(text)) {
		JS_FreeValue(ctx, object);
		return text;
	}
	failed = JS_SetPropertyStr(ctx, object, "ok",
				   JS_NewBool(ctx, status >= 200 && status < 300)) < 0 ||
		 JS_SetPropertyStr(ctx, object, "status", JS_NewInt32(ctx, status)) < 0 ||
		 JS_SetPropertyStr(ctx, object, "url", JS_NewString(ctx, url ? url : "")) < 0 ||
		 JS_SetPropertyStr(ctx, object, "body", JS_DupValue(ctx, text)) < 0 ||
		 JS_SetPropertyStr(ctx, object, "content", JS_DupValue(ctx, text)) < 0 ||
		 JS_SetPropertyStr(ctx, object, "length", JS_NewInt64(ctx, (int64_t)len)) < 0 ||
		 JS_SetPropertyStr(ctx, object, "_body", JS_DupValue(ctx, text)) < 0 ||
		 JS_SetPropertyStr(ctx, object, "text",
				   JS_NewCFunction(ctx, js_fetch_text, "text", 0)) < 0 ||
		 JS_SetPropertyStr(ctx, object, "toString",
				   JS_NewCFunction(ctx, js_fetch_to_string, "toString", 0)) < 0 ||
		 JS_SetPropertyStr(ctx, object, "valueOf",
				   JS_NewCFunction(ctx, js_fetch_to_string, "valueOf", 0)) < 0 ||
		 JS_SetPropertyStr(ctx, object, "slice",
				   JS_NewCFunction(ctx, js_fetch_slice, "slice", 2)) < 0 ||
		 JS_SetPropertyStr(ctx, object, "substring",
				   JS_NewCFunction(ctx, js_fetch_substring, "substring", 2)) < 0 ||
		 JS_SetPropertyStr(ctx, object, "indexOf",
				   JS_NewCFunction(ctx, js_fetch_index_of, "indexOf", 1)) < 0;
	JS_FreeValue(ctx, text);
	if (failed) {
		JS_FreeValue(ctx, object);
		return JS_EXCEPTION;
	}
	return object;
}

static int fetch_interrupted(void *opaque)
{
	const struct runner_state *state = opaque;

	return state && state->deadline_ns > 0 && nb_time_remaining_ns(state->deadline_ns) == 0;
}

static JSValue js_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	struct runner_state *state = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
	struct http_control control = {
		.interrupt_check = fetch_interrupted,
		.interrupt_user_data = state,
	};
	struct http_response response = {0};
	struct fetch_headers headers NB_CLEANUP(fetch_headers_cleanup) = {
		.ctx = ctx,
		.object = JS_UNDEFINED,
	};
	const char *url;
	size_t url_len;
	JSValue value;
	int rc;

	(void)this_val;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "fetch(url) requires url");
	if (!runner_has_cap("network"))
		return runner_throw_cap(ctx, "network");
	url = JS_ToCStringLen(ctx, &url_len, argv[0]);
	if (!url)
		return JS_EXCEPTION;
	if (memchr(url, '\0', url_len)) {
		JS_FreeCString(ctx, url);
		return JS_ThrowTypeError(ctx, "fetch URL must not contain NUL bytes");
	}
	if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
		JS_FreeCString(ctx, url);
		return JS_ThrowTypeError(ctx, "fetch requires an HTTP or HTTPS URL");
	}
	if (!runner_list_allows("MORPH_DYNAMIC_ALLOWED_NETWORK", url)) {
		JS_FreeCString(ctx, url);
		return JS_ThrowTypeError(ctx, "network target denied");
	}
	rc = fetch_headers_create(&headers, argc > 1 ? argv[1] : JS_UNDEFINED);
	if (rc != 0) {
		JS_FreeCString(ctx, url);
		return JS_EXCEPTION;
	}
	rc = http_get_ex_timeout_limit(&control, url, ((void *)headers.lines.data),
				       (int)headers.lines.len, 30, runner_capture_limit(ctx),
				       &response);
	if (rc == NB_ERR_TOO_BIG)
		value = JS_ThrowRangeError(ctx, "fetch failed: response exceeded the configured "
					 "capture limit of %zu bytes; use headers with Range "
					 "to retrieve smaller ranges", runner_capture_limit(ctx));
	else if (rc != 0)
		value = JS_ThrowInternalError(
			ctx, "fetch failed: %s",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	else
		value = make_fetch_response(ctx, url, response.body.ptr ? response.body.ptr : "",
					    response.body.len, response.status_code);
	http_response_free(&response);
	JS_FreeCString(ctx, url);
	return value;
}

void js_fetch_install(JSContext *ctx, JSValueConst target)
{
	JS_SetPropertyStr(ctx, target, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 2));
}
