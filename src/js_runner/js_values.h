#ifndef MORPH_JS_VALUES_H
#define MORPH_JS_VALUES_H

#include "runner_api.h"
#include "nb/err.h"

struct js_buffer_view {
	JSContext *context;
	JSValue owner;
	const uint8_t *data;
	size_t length;
};

/* Retains the ArrayBuffer backing data until cleanup. No JS calls while using data:
 * arbitrary JS can detach the buffer. out must be zero-initialized and empty.
 * NB_ERR_UNSUPPORTED means a non-object input; object errors preserve a JS exception. */
int js_values_buffer_borrow(JSContext *ctx, JSValueConst value, struct js_buffer_view *out);
void js_buffer_view_cleanup(struct js_buffer_view *view);

struct runner_state *js_values_state_borrow(JSContext *ctx);
/* Owned QuickJS reference; caller releases with JS_FreeValue(ctx, value).
 * Retains the original value/property; does not deep-copy its contents. */
JSValue js_values_arg_or_prop_retain(JSContext *ctx, JSValueConst value, const char *name);
int js_values_object_int_prop(JSContext *ctx, JSValueConst object, const char *name, int32_t def,
			      int32_t *out);
/* Independent QuickJS string; release with JS_FreeCString(ctx, text). */
const char *js_values_object_string_prop_copy(JSContext *ctx, JSValueConst object,
					      const char *name);

#endif
