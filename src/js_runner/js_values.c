#include "js_values.h"
#include "nb/err.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/file.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct runner_state *js_values_state_borrow(JSContext *ctx)
{
	return JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
}

void js_buffer_view_cleanup(struct js_buffer_view *view)
{
	if (!view)
		return;
	if (view->context)
		JS_FreeValue(view->context, view->owner);
	memset(view, 0, sizeof(*view));
}

int js_values_buffer_borrow(JSContext *ctx, JSValueConst value, struct js_buffer_view *out)
{
	struct js_buffer_view candidate NB_CLEANUP(js_buffer_view_cleanup) = {
		.context = ctx, .owner = JS_UNDEFINED};
	JSValue offset_val;
	JSValue length_val;
	uint64_t offset = 0;
	uint64_t byte_length = 0;
	size_t buffer_len = 0;
	const uint8_t *bytes;
	int has_range = 0;

	if (!ctx || !out)
		return NB_ERR_INVAL;
	if (out->context || out->data || out->length)
		return NB_ERR_EXISTS;
	if (!JS_IsObject(value))
		return NB_ERR_UNSUPPORTED;
	candidate.owner = JS_GetPropertyStr(ctx, value, "buffer");
	if (JS_IsException(candidate.owner))
		return NB_ERR_INVAL;
	if (!JS_IsUndefined(candidate.owner)) {
		has_range = 1;
		offset_val = JS_GetPropertyStr(ctx, value, "byteOffset");
		if (JS_IsException(offset_val))
			return NB_ERR_INVAL;
		int rc = JS_ToIndex(ctx, &offset, offset_val);
		JS_FreeValue(ctx, offset_val);
		if (rc < 0)
			return NB_ERR_INVAL;
		length_val = JS_GetPropertyStr(ctx, value, "byteLength");
		if (JS_IsException(length_val))
			return NB_ERR_INVAL;
		rc = JS_ToIndex(ctx, &byte_length, length_val);
		JS_FreeValue(ctx, length_val);
		if (rc < 0)
			return NB_ERR_INVAL;
	} else {
		candidate.owner = JS_DupValue(ctx, value);
	}
	bytes = JS_GetArrayBuffer(ctx, &buffer_len, candidate.owner);
	if (!bytes && JS_HasException(ctx))
		return NB_ERR_INVAL;
	if (has_range && (offset > buffer_len || byte_length > buffer_len - offset)) {
		JS_ThrowRangeError(ctx, "typed array range is invalid");
		return NB_ERR_INVAL;
	}
	candidate.data = bytes ? bytes + (size_t)offset : (const uint8_t *)"";
	candidate.length = has_range ? (size_t)byte_length : buffer_len;
	*out = candidate;
	memset(&candidate, 0, sizeof(candidate));
	return 0;
}

JSValue js_values_arg_or_prop_retain(JSContext *ctx, JSValueConst value, const char *name)
{
	if (JS_IsObject(value)) {
		JSValue prop = JS_GetPropertyStr(ctx, value, name);

		if (!JS_IsUndefined(prop))
			return prop;
		JS_FreeValue(ctx, prop);
	}
	return JS_DupValue(ctx, value);
}

int js_values_object_int_prop(JSContext *ctx, JSValueConst object, const char *name, int32_t def,
			      int32_t *out)
{
	JSValue value;

	if (!out)
		return NB_ERR_INVAL;
	*out = def;
	if (!JS_IsObject(object))
		return 0;
	value = JS_GetPropertyStr(ctx, object, name);
	if (JS_IsUndefined(value) || JS_IsNull(value)) {
		JS_FreeValue(ctx, value);
		return 0;
	}
	if (JS_ToInt32(ctx, out, value) < 0) {
		JS_FreeValue(ctx, value);
		return NB_ERR_INVAL;
	}
	JS_FreeValue(ctx, value);
	return 0;
}

const char *js_values_object_string_prop_copy(JSContext *ctx, JSValueConst object, const char *name)
{
	JSValue value;
	const char *out;

	if (!JS_IsObject(object))
		return NULL;
	value = JS_GetPropertyStr(ctx, object, name);
	if (JS_IsUndefined(value) || JS_IsNull(value)) {
		JS_FreeValue(ctx, value);
		return NULL;
	}
	out = JS_ToCString(ctx, value);
	JS_FreeValue(ctx, value);
	return out;
}
