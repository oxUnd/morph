#ifndef MORPH_JS_VALUES_H
#define MORPH_JS_VALUES_H

#include "runner_api.h"

struct runner_state *js_values_state(JSContext *ctx);
uint8_t *js_values_buffer_bytes(JSContext *ctx, JSValueConst value, size_t *len);
JSValue js_values_get_arg_or_prop(JSContext *ctx, JSValueConst value, const char *name);
int js_values_object_int_prop(JSContext *ctx, JSValueConst object, const char *name, int32_t def,
			      int32_t *out);
const char *js_values_object_string_prop(JSContext *ctx, JSValueConst object, const char *name);

#endif
