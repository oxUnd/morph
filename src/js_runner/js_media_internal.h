#ifndef MORPH_JS_MEDIA_INTERNAL_H
#define MORPH_JS_MEDIA_INTERNAL_H

#include "runner_api.h"
#include <cairo.h>
#include <vips/vips.h>

struct sharp_image {
	VipsImage *image;
	char format[16];
};

struct canvas {
	cairo_surface_t *surface;
	cairo_t *cr;
	unsigned char *data;
	int width;
	int height;
	int stride;
};

struct image_data {
	unsigned char *data;
	int width;
	int height;
};

#endif
