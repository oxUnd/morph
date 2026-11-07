#ifndef MORPH_JS_IMAGE_H
#define MORPH_JS_IMAGE_H

#include "runner_api.h"
#include "js_media_internal.h"

int js_image_require(JSContext *ctx);
int js_image_path_ext_is(const char *path, const char *ext);
JSValue js_image_open_value(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
void js_image_install(JSContext *ctx, JSValueConst morph);

#endif
