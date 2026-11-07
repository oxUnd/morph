#ifndef MORPH_JS_CANVAS_H
#define MORPH_JS_CANVAS_H

#include "runner_api.h"
#include "js_media_internal.h"

int js_canvas_write_surface(cairo_surface_t *surface, const char *path);
cairo_surface_t *js_canvas_image_surface(struct sharp_image *img);
void js_canvas_install(JSContext *ctx, JSValueConst morph);

#endif
