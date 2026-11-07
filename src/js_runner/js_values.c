#include "js_values.h"
#include "util/buf.h"
#include "util/file.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct runner_state *js_values_state(JSContext *ctx)
{
	return JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
}

uint8_t *js_values_buffer_bytes(JSContext *ctx, JSValueConst value, size_t *len)
{
	JSValue buffer;
	JSValue offset_val;
	JSValue length_val;
	uint64_t offset = 0;
	uint64_t byte_length = 0;
	size_t buffer_len = 0;
	uint8_t *bytes;

	if (!len)
		return NULL;
	*len = 0;
	if (!JS_IsObject(value))
		return NULL;
	buffer = JS_GetPropertyStr(ctx, value, "buffer");
	if (!JS_IsUndefined(buffer)) {
		offset_val = JS_GetPropertyStr(ctx, value, "byteOffset");
		length_val = JS_GetPropertyStr(ctx, value, "byteLength");
		(void)JS_ToIndex(ctx, &offset, offset_val);
		(void)JS_ToIndex(ctx, &byte_length, length_val);
		JS_FreeValue(ctx, offset_val);
		JS_FreeValue(ctx, length_val);
		bytes = JS_GetArrayBuffer(ctx, &buffer_len, buffer);
		JS_FreeValue(ctx, buffer);
		if (!bytes)
			return NULL;
		if (offset > buffer_len || byte_length > buffer_len - offset) {
			JS_ThrowRangeError(ctx, "typed array range is invalid");
			return NULL;
		}
		*len = (size_t)byte_length;
		return bytes + (size_t)offset;
	}
	JS_FreeValue(ctx, buffer);
	bytes = JS_GetArrayBuffer(ctx, &buffer_len, value);
	if (!bytes)
		return NULL;
	*len = buffer_len;
	return bytes;
}

JSValue js_values_get_arg_or_prop(JSContext *ctx, JSValueConst value, const char *name)
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
		return -EINVAL;
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
		return -EINVAL;
	}
	JS_FreeValue(ctx, value);
	return 0;
}

const char *js_values_object_string_prop(JSContext *ctx, JSValueConst object, const char *name)
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
