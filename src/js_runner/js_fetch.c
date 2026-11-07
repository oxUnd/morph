#include "js_fetch.h"
#include "runner_api.h"
#include "http/client.h"
#include "util/error.h"

#include <errno.h>
#include <string.h>
#include <time.h>

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

	return state && state->deadline > 0 && time(NULL) >= state->deadline;
}

static JSValue js_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	struct runner_state *state = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
	struct http_control control = {
		.interrupt_check = fetch_interrupted,
		.interrupt_user_data = state,
	};
	struct http_response response = {0};
	const char *url;
	JSValue value;
	int rc;

	(void)this_val;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "fetch(url) requires url");
	if (!runner_has_cap("network"))
		return runner_throw_cap(ctx, "network");
	url = JS_ToCString(ctx, argv[0]);
	if (!url)
		return JS_EXCEPTION;
	if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
		JS_FreeCString(ctx, url);
		return JS_ThrowTypeError(ctx, "fetch requires an HTTP or HTTPS URL");
	}
	if (!runner_list_allows("MORPH_DYNAMIC_ALLOWED_NETWORK", url)) {
		JS_FreeCString(ctx, url);
		return JS_ThrowTypeError(ctx, "network target denied");
	}
	rc = http_get_ex_timeout_limit(&control, url, NULL, 0, 30, RUNNER_MAX_CAPTURE, &response);
	if (rc != 0)
		value = JS_ThrowInternalError(ctx, "fetch failed: %s", morph_strerror(rc));
	else
		value = make_fetch_response(ctx, url, response.body.data ? response.body.data : "",
					    response.body.len, response.status_code);
	http_response_free(&response);
	JS_FreeCString(ctx, url);
	return value;
}

void js_fetch_install(JSContext *ctx, JSValueConst target)
{
	JS_SetPropertyStr(ctx, target, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 1));
}
