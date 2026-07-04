#include "runner_api.h"
#include "util/buf.h"
#include "util/file.h"
#include <cairo.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vips/vips.h>
#include "wasm3.h"

#ifdef __ANDROID__
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image.h"
#include "stb_image_write.h"

#define WASM_STACK_SIZE (64 * 1024)
#define WASM_MAX_EXPORTS 128

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

struct wasm_module {
	uint8_t *bytes;
	size_t len;
	char exports[WASM_MAX_EXPORTS][64];
	int export_count;
	int has_memory;
};

struct wasm_instance {
	IM3Environment env;
	IM3Runtime runtime;
	uint8_t *bytes;
	size_t len;
	char exports[WASM_MAX_EXPORTS][64];
	int export_count;
};

static JSClassID sharp_class_id;
static JSClassID canvas_class_id;
static JSClassID image_data_class_id;
static JSClassID wasm_module_class_id;
static JSClassID wasm_instance_class_id;
static int classes_inited;

static VipsImage *image_from_stb_pixels(JSContext *ctx, unsigned char *pixels,
					int width, int height);
static int write_image_stb(const char *path, VipsImage *image);

static int require_image(JSContext *ctx)
{
	if (!runner_has_cap("image")) {
		runner_throw_cap(ctx, "image");
		return 0;
	}
	return 1;
}

static int require_wasm(JSContext *ctx)
{
	if (!runner_has_cap("wasm")) {
		runner_throw_cap(ctx, "wasm");
		return 0;
	}
	return 1;
}

static int path_allowed(JSContext *ctx, const char *path, int write)
{
	const char *cap = write ? "fs_write" : "fs_read";
	const char *env = write ? "MORPH_DYNAMIC_ALLOWED_WRITE" :
		"MORPH_DYNAMIC_ALLOWED_READ";

	if (!runner_has_cap(cap)) {
		runner_throw_cap(ctx, cap);
		return 0;
	}
	if (!runner_list_allows(env, path)) {
		JS_ThrowTypeError(ctx, "%s path denied", write ? "write" : "read");
		return 0;
	}
	return 1;
}

static void sharp_finalizer(JSRuntime *rt, JSValue val)
{
	struct sharp_image *img = JS_GetOpaque(val, sharp_class_id);

	(void)rt;
	if (!img)
		return;
	if (img->image)
		g_object_unref(img->image);
	free(img);
}

static void canvas_finalizer(JSRuntime *rt, JSValue val)
{
	struct canvas *canvas = JS_GetOpaque(val, canvas_class_id);

	(void)rt;
	if (!canvas)
		return;
	if (canvas->cr)
		cairo_destroy(canvas->cr);
	if (canvas->surface)
		cairo_surface_destroy(canvas->surface);
	free(canvas->data);
	free(canvas);
}

static void image_data_finalizer(JSRuntime *rt, JSValue val)
{
	struct image_data *data = JS_GetOpaque(val, image_data_class_id);

	(void)rt;
	if (!data)
		return;
	free(data->data);
	free(data);
}

static void wasm_module_finalizer(JSRuntime *rt, JSValue val)
{
	struct wasm_module *module = JS_GetOpaque(val, wasm_module_class_id);

	(void)rt;
	if (!module)
		return;
	free(module->bytes);
	free(module);
}

static void wasm_instance_finalizer(JSRuntime *rt, JSValue val)
{
	struct wasm_instance *inst = JS_GetOpaque(val, wasm_instance_class_id);

	(void)rt;
	if (!inst)
		return;
	if (inst->runtime)
		m3_FreeRuntime(inst->runtime);
	if (inst->env)
		m3_FreeEnvironment(inst->env);
	free(inst->bytes);
	free(inst);
}

static JSValue new_sharp_object(JSContext *ctx, VipsImage *image,
				const char *format)
{
	struct sharp_image *img;
	JSValue obj;

	img = calloc(1, sizeof(*img));
	if (!img)
		return JS_ThrowOutOfMemory(ctx);
	img->image = image;
	strncpy(img->format, format ? format : ".png", sizeof(img->format) - 1);
	obj = JS_NewObjectClass(ctx, sharp_class_id);
	if (JS_IsException(obj)) {
		if (img->image)
			g_object_unref(img->image);
		free(img);
		return obj;
	}
	JS_SetOpaque(obj, img);
	return obj;
}

static JSValue throw_vips_error(JSContext *ctx, const char *prefix)
{
	const char *detail = vips_error_buffer();
	JSValue err;

	if (detail && *detail)
		err = JS_ThrowInternalError(ctx, "%s: %s", prefix, detail);
	else
		err = JS_ThrowInternalError(ctx, "%s", prefix);
	vips_error_clear();
	return err;
}

static uint8_t *js_buffer_bytes(JSContext *ctx, JSValueConst value,
				size_t *len)
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

static VipsImage *sharp_load_input(JSContext *ctx, JSValueConst value)
{
	const char *path;
	uint8_t *bytes;
	size_t len;
	VipsImage *image;

	if (JS_IsString(value)) {
		path = JS_ToCString(ctx, value);
		if (!path)
			return NULL;
		if (!path_allowed(ctx, path, 0)) {
			JS_FreeCString(ctx, path);
			return NULL;
		}
		image = vips_image_new_from_file(path, "access",
						 VIPS_ACCESS_SEQUENTIAL,
						 NULL);
		if (!image) {
			int width = 0;
			int height = 0;
			int channels = 0;
			unsigned char *pixels;

			vips_error_clear();
			pixels = stbi_load(path, &width, &height, &channels, 4);
			image = image_from_stb_pixels(ctx, pixels, width,
						      height);
		}
		JS_FreeCString(ctx, path);
		if (!image)
			(void)throw_vips_error(ctx, "failed to load image");
		return image;
	}
	bytes = js_buffer_bytes(ctx, value, &len);
	if (!bytes) {
		if (!JS_IsObject(value))
			JS_ThrowTypeError(ctx,
					  "sharp(input) requires a path string, "
					  "ArrayBuffer, or typed array");
		return NULL;
	}
	image = vips_image_new_from_buffer(bytes, len, "", "access",
					   VIPS_ACCESS_SEQUENTIAL, NULL);
	if (!image && len <= (size_t)INT_MAX) {
		int width = 0;
		int height = 0;
		int channels = 0;
		unsigned char *pixels;

		vips_error_clear();
		pixels = stbi_load_from_memory(bytes, (int)len, &width,
					       &height, &channels, 4);
		image = image_from_stb_pixels(ctx, pixels, width, height);
	}
	if (!image)
		(void)throw_vips_error(ctx, "failed to load image buffer");
	return image;
}

static VipsImage *image_from_stb_pixels(JSContext *ctx, unsigned char *pixels,
					int width, int height)
{
	VipsImage *image;
	size_t len;

	if (!pixels)
		return NULL;
	if (width <= 0 || height <= 0) {
		stbi_image_free(pixels);
		return NULL;
	}
	len = (size_t)width * (size_t)height * 4U;
	image = vips_image_new_from_memory_copy(pixels, len, width, height,
						4, VIPS_FORMAT_UCHAR);
	stbi_image_free(pixels);
	if (!image)
		(void)throw_vips_error(ctx, "failed to create image from pixels");
	return image;
}

static int path_ext_is(const char *path, const char *ext)
{
	const char *dot;

	if (!path || !ext)
		return 0;
	dot = strrchr(path, '.');
	if (!dot)
		return 0;
	while (*dot && *ext) {
		char a = *dot;
		char b = *ext;

		if (a >= 'A' && a <= 'Z')
			a = (char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z')
			b = (char)(b - 'A' + 'a');
		if (a != b)
			return 0;
		dot++;
		ext++;
	}
	return *dot == '\0' && *ext == '\0';
}

static int write_image_stb(const char *path, VipsImage *image)
{
	void *mem;
	size_t len;
	int width;
	int height;
	int bands;
	int ok = 0;

	if (!path || !image)
		return -EINVAL;
	width = vips_image_get_width(image);
	height = vips_image_get_height(image);
	bands = vips_image_get_bands(image);
	if (width <= 0 || height <= 0 || bands <= 0 || bands > 4)
		return -EINVAL;
	mem = vips_image_write_to_memory(image, &len);
	if (!mem)
		return -EIO;
	if (path_ext_is(path, ".jpg") || path_ext_is(path, ".jpeg")) {
		if (bands == 4) {
			unsigned char *src = mem;
			unsigned char *rgb;
			size_t pixels = (size_t)width * (size_t)height;

			rgb = malloc(pixels * 3U);
			if (rgb) {
				for (size_t i = 0; i < pixels; i++) {
					rgb[i * 3U] = src[i * 4U];
					rgb[i * 3U + 1U] = src[i * 4U + 1U];
					rgb[i * 3U + 2U] = src[i * 4U + 2U];
				}
				ok = stbi_write_jpg(path, width, height, 3,
						    rgb, 90);
				free(rgb);
			}
		} else {
			ok = stbi_write_jpg(path, width, height, bands, mem, 90);
		}
	} else {
		ok = stbi_write_png(path, width, height, bands, mem,
				    width * bands);
	}
	g_free(mem);
	return ok ? 0 : -EIO;
}

static int parse_js_color(JSContext *ctx, JSValueConst value,
			  double color[4])
{
	const char *s;
	unsigned int rv;
	unsigned int gv;
	unsigned int bv;

	if (!color)
		return -EINVAL;
	color[0] = 0.0;
	color[1] = 0.0;
	color[2] = 0.0;
	color[3] = 255.0;
	if (JS_IsString(value)) {
		s = JS_ToCString(ctx, value);
		if (!s)
			return -EINVAL;
		if (strcmp(s, "white") == 0) {
			color[0] = 255.0;
			color[1] = 255.0;
			color[2] = 255.0;
		} else if (strcmp(s, "black") == 0) {
			color[0] = 0.0;
			color[1] = 0.0;
			color[2] = 0.0;
		} else if (s[0] == '#' && strlen(s) == 7 &&
			   sscanf(s + 1, "%02x%02x%02x", &rv, &gv, &bv) == 3) {
			color[0] = (double)rv;
			color[1] = (double)gv;
			color[2] = (double)bv;
		}
		JS_FreeCString(ctx, s);
		return 0;
	}
	if (JS_IsObject(value)) {
		JSValue v;

		v = JS_GetPropertyStr(ctx, value, "r");
		(void)JS_ToFloat64(ctx, &color[0], v);
		JS_FreeValue(ctx, v);
		v = JS_GetPropertyStr(ctx, value, "g");
		(void)JS_ToFloat64(ctx, &color[1], v);
		JS_FreeValue(ctx, v);
		v = JS_GetPropertyStr(ctx, value, "b");
		(void)JS_ToFloat64(ctx, &color[2], v);
		JS_FreeValue(ctx, v);
		v = JS_GetPropertyStr(ctx, value, "alpha");
		if (!JS_IsUndefined(v))
			(void)JS_ToFloat64(ctx, &color[3], v);
		JS_FreeValue(ctx, v);
		if (color[3] <= 1.0)
			color[3] *= 255.0;
		return 0;
	}
	return 0;
}

static unsigned char clamp_color(double value)
{
	if (value <= 0.0)
		return 0;
	if (value >= 255.0)
		return 255;
	return (unsigned char)(value + 0.5);
}

static VipsImage *sharp_create_input(JSContext *ctx, JSValueConst create)
{
	JSValue v = JS_UNDEFINED;
	VipsImage *image = NULL;
	unsigned char *data = NULL;
	double color[4];
	size_t pixels;
	size_t len;
	int32_t width = 0;
	int32_t height = 0;
	int32_t channels = 4;
	unsigned char r;
	unsigned char g;
	unsigned char b;
	unsigned char a;

	if (!JS_IsObject(create)) {
		JS_ThrowTypeError(ctx,
			"sharp({create}) requires create to be an object with "
			"width, height, optional channels, and background");
		return NULL;
	}
	v = JS_GetPropertyStr(ctx, create, "width");
	(void)JS_ToInt32(ctx, &width, v);
	JS_FreeValue(ctx, v);
	v = JS_GetPropertyStr(ctx, create, "height");
	(void)JS_ToInt32(ctx, &height, v);
	JS_FreeValue(ctx, v);
	v = JS_GetPropertyStr(ctx, create, "channels");
	if (!JS_IsUndefined(v))
		(void)JS_ToInt32(ctx, &channels, v);
	JS_FreeValue(ctx, v);
	if (width <= 0 || height <= 0 || width > 32768 || height > 32768) {
		(void)JS_ThrowRangeError(ctx,
			"sharp({create}) invalid size: width and height must "
			"be positive integers up to 32768");
		return NULL;
	}
	if (channels < 1 || channels > 4) {
		(void)JS_ThrowRangeError(ctx,
			"sharp({create}) invalid channels: expected 1, 2, 3, "
			"or 4");
		return NULL;
	}
	v = JS_GetPropertyStr(ctx, create, "background");
	(void)parse_js_color(ctx, v, color);
	JS_FreeValue(ctx, v);
	r = clamp_color(color[0]);
	g = clamp_color(color[1]);
	b = clamp_color(color[2]);
	a = clamp_color(color[3]);
	pixels = (size_t)width * (size_t)height;
	if (pixels > SIZE_MAX / (size_t)channels) {
		(void)JS_ThrowRangeError(ctx, "sharp({create}) image too large");
		return NULL;
	}
	len = pixels * (size_t)channels;
	data = malloc(len);
	if (!data) {
		(void)JS_ThrowOutOfMemory(ctx);
		return NULL;
	}
	for (size_t i = 0; i < pixels; i++) {
		size_t off = i * (size_t)channels;
		if (channels == 1) {
			data[off] = (unsigned char)(((int)r + (int)g +
						     (int)b) / 3);
		} else if (channels == 2) {
			data[off] = (unsigned char)(((int)r + (int)g +
						     (int)b) / 3);
			data[off + 1] = a;
		} else if (channels == 3) {
			data[off] = r;
			data[off + 1] = g;
			data[off + 2] = b;
		} else {
			data[off] = r;
			data[off + 1] = g;
			data[off + 2] = b;
			data[off + 3] = a;
		}
	}
	image = vips_image_new_from_memory_copy(data, len, width, height,
						channels, VIPS_FORMAT_UCHAR);
	free(data);
	if (!image)
		(void)throw_vips_error(ctx, "failed to create image");
	return image;
}

static JSValue new_canvas_object(JSContext *ctx, int width, int height)
{
	struct canvas *canvas;
	JSValue obj;

	if (width <= 0 || height <= 0 || width > 32768 || height > 32768)
		return JS_ThrowRangeError(ctx, "invalid canvas size");
	canvas = calloc(1, sizeof(*canvas));
	if (!canvas)
		return JS_ThrowOutOfMemory(ctx);
	canvas->width = width;
	canvas->height = height;
	canvas->stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32,
							width);
	if (canvas->stride <= 0)
		goto fail;
	canvas->data = calloc((size_t)canvas->stride, (size_t)height);
	if (!canvas->data)
		goto fail;
	canvas->surface = cairo_image_surface_create_for_data(
		canvas->data, CAIRO_FORMAT_ARGB32, width, height,
		canvas->stride);
	if (cairo_surface_status(canvas->surface) != CAIRO_STATUS_SUCCESS)
		goto fail;
	canvas->cr = cairo_create(canvas->surface);
	if (cairo_status(canvas->cr) != CAIRO_STATUS_SUCCESS)
		goto fail;
	obj = JS_NewObjectClass(ctx, canvas_class_id);
	if (JS_IsException(obj))
		goto fail_value;
	JS_SetOpaque(obj, canvas);
	return obj;
fail_value:
	canvas_finalizer(JS_GetRuntime(ctx), obj);
	return obj;
fail:
	if (canvas->cr)
		cairo_destroy(canvas->cr);
	if (canvas->surface)
		cairo_surface_destroy(canvas->surface);
	free(canvas->data);
	free(canvas);
	return JS_ThrowOutOfMemory(ctx);
}

static JSValue js_sharp_call(JSContext *ctx, JSValueConst this_val,
			     int argc, JSValueConst *argv)
{
	VipsImage *image;
	JSValue create = JS_UNDEFINED;

	(void)this_val;
	if (!require_image(ctx))
		return JS_EXCEPTION;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "sharp(input) requires input");
	if (JS_IsObject(argv[0])) {
		create = JS_GetPropertyStr(ctx, argv[0], "create");
		if (!JS_IsUndefined(create)) {
			image = sharp_create_input(ctx, create);
			JS_FreeValue(ctx, create);
			if (!image)
				return JS_EXCEPTION;
			return new_sharp_object(ctx, image, ".png");
		}
		JS_FreeValue(ctx, create);
	}
	image = sharp_load_input(ctx, argv[0]);
	if (!image)
		return JS_EXCEPTION;
	return new_sharp_object(ctx, image, ".png");
}

static struct sharp_image *sharp_this(JSContext *ctx, JSValueConst val)
{
	struct sharp_image *img = JS_GetOpaque2(ctx, val, sharp_class_id);

	return img;
}

static JSValue sharp_replace(JSContext *ctx, JSValueConst this_val,
			     VipsImage *next)
{
	struct sharp_image *img = sharp_this(ctx, this_val);

	if (!img)
		return JS_EXCEPTION;
	if (!next)
		return JS_ThrowInternalError(ctx, "image operation failed");
	if (img->image)
		g_object_unref(img->image);
	img->image = next;
	return JS_DupValue(ctx, this_val);
}

static JSValue js_sharp_metadata(JSContext *ctx, JSValueConst this_val,
				 int argc, JSValueConst *argv)
{
	struct sharp_image *img = sharp_this(ctx, this_val);
	JSValue obj;

	(void)argc;
	(void)argv;
	if (!img)
		return JS_EXCEPTION;
	obj = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, obj, "width",
			  JS_NewInt32(ctx, vips_image_get_width(img->image)));
	JS_SetPropertyStr(ctx, obj, "height",
			  JS_NewInt32(ctx, vips_image_get_height(img->image)));
	JS_SetPropertyStr(ctx, obj, "channels",
			  JS_NewInt32(ctx, vips_image_get_bands(img->image)));
	JS_SetPropertyStr(ctx, obj, "format", JS_NewString(ctx, img->format));
	return obj;
}

static JSValue js_sharp_resize(JSContext *ctx, JSValueConst this_val,
			       int argc, JSValueConst *argv)
{
	struct sharp_image *img = sharp_this(ctx, this_val);
	VipsImage *next = NULL;
	double scale;
	int32_t width = 0;
	int32_t height = 0;
	int ow;
	int oh;

	if (!img)
		return JS_EXCEPTION;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "resize(width[, height])");
	if (JS_IsObject(argv[0])) {
		JSValue v = JS_GetPropertyStr(ctx, argv[0], "width");
		(void)JS_ToInt32(ctx, &width, v);
		JS_FreeValue(ctx, v);
		v = JS_GetPropertyStr(ctx, argv[0], "height");
		(void)JS_ToInt32(ctx, &height, v);
		JS_FreeValue(ctx, v);
	} else {
		(void)JS_ToInt32(ctx, &width, argv[0]);
		if (argc > 1)
			(void)JS_ToInt32(ctx, &height, argv[1]);
	}
	ow = vips_image_get_width(img->image);
	oh = vips_image_get_height(img->image);
	if (width <= 0 && height <= 0)
		return JS_ThrowRangeError(ctx, "invalid resize size");
	if (width <= 0)
		scale = (double)height / (double)oh;
	else if (height <= 0)
		scale = (double)width / (double)ow;
	else
		scale = fmin((double)width / (double)ow,
			     (double)height / (double)oh);
	if (scale <= 0.0)
		return JS_ThrowRangeError(ctx, "invalid resize scale");
	if (vips_resize(img->image, &next, scale, NULL) != 0)
		next = NULL;
	return sharp_replace(ctx, this_val, next);
}

static JSValue js_sharp_extract(JSContext *ctx, JSValueConst this_val,
				int argc, JSValueConst *argv)
{
	struct sharp_image *img = sharp_this(ctx, this_val);
	VipsImage *next = NULL;
	JSValue v;
	int32_t left = 0;
	int32_t top = 0;
	int32_t width = 0;
	int32_t height = 0;

	if (!img)
		return JS_EXCEPTION;
	if (argc < 1 || !JS_IsObject(argv[0]))
		return JS_ThrowTypeError(ctx, "extract(options)");
	v = JS_GetPropertyStr(ctx, argv[0], "left");
	(void)JS_ToInt32(ctx, &left, v);
	JS_FreeValue(ctx, v);
	v = JS_GetPropertyStr(ctx, argv[0], "top");
	(void)JS_ToInt32(ctx, &top, v);
	JS_FreeValue(ctx, v);
	v = JS_GetPropertyStr(ctx, argv[0], "width");
	(void)JS_ToInt32(ctx, &width, v);
	JS_FreeValue(ctx, v);
	v = JS_GetPropertyStr(ctx, argv[0], "height");
	(void)JS_ToInt32(ctx, &height, v);
	JS_FreeValue(ctx, v);
	if (width <= 0 || height <= 0)
		return JS_ThrowRangeError(ctx, "invalid extract size");
	if (vips_extract_area(img->image, &next, left, top, width, height,
			      NULL) != 0)
		next = NULL;
	return sharp_replace(ctx, this_val, next);
}

static JSValue js_sharp_extend(JSContext *ctx, JSValueConst this_val,
			       int argc, JSValueConst *argv)
{
	struct sharp_image *img = sharp_this(ctx, this_val);
	VipsImage *next = NULL;
	JSValue v;
	int32_t top = 0;
	int32_t bottom = 0;
	int32_t left = 0;
	int32_t right = 0;
	int width;
	int height;
	double background[4];
	VipsArrayDouble *background_array;

	if (!img)
		return JS_EXCEPTION;
	if (argc < 1 || !JS_IsObject(argv[0]))
		return JS_ThrowTypeError(ctx, "extend(options)");
	v = JS_GetPropertyStr(ctx, argv[0], "top");
	(void)JS_ToInt32(ctx, &top, v);
	JS_FreeValue(ctx, v);
	v = JS_GetPropertyStr(ctx, argv[0], "bottom");
	(void)JS_ToInt32(ctx, &bottom, v);
	JS_FreeValue(ctx, v);
	v = JS_GetPropertyStr(ctx, argv[0], "left");
	(void)JS_ToInt32(ctx, &left, v);
	JS_FreeValue(ctx, v);
	v = JS_GetPropertyStr(ctx, argv[0], "right");
	(void)JS_ToInt32(ctx, &right, v);
	JS_FreeValue(ctx, v);
	if (top < 0 || bottom < 0 || left < 0 || right < 0)
		return JS_ThrowRangeError(ctx, "invalid extend size");
	width = vips_image_get_width(img->image) + left + right;
	height = vips_image_get_height(img->image) + top + bottom;
	v = JS_GetPropertyStr(ctx, argv[0], "background");
	(void)parse_js_color(ctx, v, background);
	JS_FreeValue(ctx, v);
	background_array = vips_array_double_new(background, 4);
	if (!background_array)
		return JS_ThrowOutOfMemory(ctx);
	if (vips_embed(img->image, &next, left, top, width, height,
		       "extend", VIPS_EXTEND_BACKGROUND, "background",
		       background_array, NULL) != 0) {
		VipsArea *area = (VipsArea *)background_array;
		vips_area_unref(area);
		return throw_vips_error(ctx, "failed to extend image");
	}
	{
		VipsArea *area = (VipsArea *)background_array;
		vips_area_unref(area);
	}
	return sharp_replace(ctx, this_val, next);
}

static JSValue js_sharp_rotate(JSContext *ctx, JSValueConst this_val,
			       int argc, JSValueConst *argv)
{
	struct sharp_image *img = sharp_this(ctx, this_val);
	VipsImage *next = NULL;
	double raw_angle = 90.0;
	int angle;

	if (!img)
		return JS_EXCEPTION;
	if (argc > 0)
		(void)JS_ToFloat64(ctx, &raw_angle, argv[0]);
	angle = ((int)raw_angle % 360 + 360) % 360;
	if (angle == 0)
		return JS_DupValue(ctx, this_val);
	if (angle == 90)
		(void)vips_rot(img->image, &next, VIPS_ANGLE_D90, NULL);
	else if (angle == 180)
		(void)vips_rot(img->image, &next, VIPS_ANGLE_D180, NULL);
	else if (angle == 270)
		(void)vips_rot(img->image, &next, VIPS_ANGLE_D270, NULL);
	else if (vips_similarity(img->image, &next, "angle", raw_angle,
				 NULL) != 0)
		return throw_vips_error(ctx, "failed to rotate image");
	return sharp_replace(ctx, this_val, next);
}

static JSValue js_sharp_blur(JSContext *ctx, JSValueConst this_val,
			     int argc, JSValueConst *argv)
{
	struct sharp_image *img = sharp_this(ctx, this_val);
	VipsImage *next = NULL;
	double sigma = 1.0;

	if (!img)
		return JS_EXCEPTION;
	if (argc > 0)
		(void)JS_ToFloat64(ctx, &sigma, argv[0]);
	if (sigma <= 0.0)
		sigma = 1.0;
	if (vips_gaussblur(img->image, &next, sigma, NULL) != 0)
		next = NULL;
	return sharp_replace(ctx, this_val, next);
}

static JSValue js_sharp_sharpen(JSContext *ctx, JSValueConst this_val,
				int argc, JSValueConst *argv)
{
	struct sharp_image *img = sharp_this(ctx, this_val);
	VipsImage *next = NULL;

	(void)argc;
	(void)argv;
	if (!img)
		return JS_EXCEPTION;
	if (vips_sharpen(img->image, &next, NULL) != 0)
		next = NULL;
	return sharp_replace(ctx, this_val, next);
}

static JSValue js_sharp_grayscale(JSContext *ctx, JSValueConst this_val,
				  int argc, JSValueConst *argv)
{
	struct sharp_image *img = sharp_this(ctx, this_val);
	VipsImage *next = NULL;

	(void)argc;
	(void)argv;
	if (!img)
		return JS_EXCEPTION;
	if (vips_colourspace(img->image, &next, VIPS_INTERPRETATION_B_W,
			     NULL) != 0)
		next = NULL;
	return sharp_replace(ctx, this_val, next);
}

static JSValue js_sharp_flatten(JSContext *ctx, JSValueConst this_val,
				int argc, JSValueConst *argv)
{
	struct sharp_image *img = sharp_this(ctx, this_val);
	VipsImage *next = NULL;

	(void)argc;
	(void)argv;
	if (!img)
		return JS_EXCEPTION;
	if (vips_flatten(img->image, &next, NULL) != 0)
		next = NULL;
	return sharp_replace(ctx, this_val, next);
}

static JSValue js_sharp_composite(JSContext *ctx, JSValueConst this_val,
				  int argc, JSValueConst *argv)
{
	struct sharp_image *img = sharp_this(ctx, this_val);
	JSValue len_val;
	uint32_t len = 0;

	if (!img)
		return JS_EXCEPTION;
	if (argc < 1 || !JS_IsArray(ctx, argv[0]))
		return JS_ThrowTypeError(ctx, "composite(overlays)");
	len_val = JS_GetPropertyStr(ctx, argv[0], "length");
	if (JS_ToUint32(ctx, &len, len_val) < 0) {
		JS_FreeValue(ctx, len_val);
		return JS_EXCEPTION;
	}
	JS_FreeValue(ctx, len_val);
	for (uint32_t i = 0; i < len; i++) {
		JSValue item = JS_UNDEFINED;
		JSValue input = JS_UNDEFINED;
		JSValue left_val = JS_UNDEFINED;
		JSValue top_val = JS_UNDEFINED;
		struct sharp_image *overlay_sharp;
		VipsImage *overlay = NULL;
		VipsImage *next = NULL;
		int32_t left = 0;
		int32_t top = 0;

		item = JS_GetPropertyUint32(ctx, argv[0], i);
		if (JS_IsException(item))
			return item;
		if (!JS_IsObject(item)) {
			JS_FreeValue(ctx, item);
			return JS_ThrowTypeError(ctx,
						 "composite overlay must be object");
		}
		input = JS_GetPropertyStr(ctx, item, "input");
		if (JS_IsUndefined(input)) {
			JS_FreeValue(ctx, input);
			JS_FreeValue(ctx, item);
			return JS_ThrowTypeError(ctx,
						 "composite overlay requires input");
		}
		left_val = JS_GetPropertyStr(ctx, item, "left");
		top_val = JS_GetPropertyStr(ctx, item, "top");
		(void)JS_ToInt32(ctx, &left, left_val);
		(void)JS_ToInt32(ctx, &top, top_val);
		JS_FreeValue(ctx, left_val);
		JS_FreeValue(ctx, top_val);
		overlay_sharp = JS_GetOpaque(input, sharp_class_id);
		if (overlay_sharp && overlay_sharp->image)
			overlay = g_object_ref(overlay_sharp->image);
		else
			overlay = sharp_load_input(ctx, input);
		JS_FreeValue(ctx, input);
		JS_FreeValue(ctx, item);
		if (!overlay)
			return JS_EXCEPTION;
		if (vips_composite2(img->image, overlay, &next,
				    VIPS_BLEND_MODE_OVER, "x", left, "y",
				    top, NULL) != 0) {
			g_object_unref(overlay);
			return throw_vips_error(ctx, "failed to composite image");
		}
		g_object_unref(overlay);
		if (img->image)
			g_object_unref(img->image);
		img->image = next;
	}
	return JS_DupValue(ctx, this_val);
}

static JSValue js_sharp_format(JSContext *ctx, JSValueConst this_val,
			       int argc, JSValueConst *argv, int magic)
{
	struct sharp_image *img = sharp_this(ctx, this_val);

	(void)argc;
	(void)argv;
	if (!img)
		return JS_EXCEPTION;
	if (magic == 1)
		strncpy(img->format, ".jpg", sizeof(img->format) - 1);
	else if (magic == 2)
		strncpy(img->format, ".webp", sizeof(img->format) - 1);
	else
		strncpy(img->format, ".png", sizeof(img->format) - 1);
	return JS_DupValue(ctx, this_val);
}

static JSValue js_sharp_to_file(JSContext *ctx, JSValueConst this_val,
				int argc, JSValueConst *argv)
{
	struct sharp_image *img = sharp_this(ctx, this_val);
	const char *path;
	int rc;

	if (!img)
		return JS_EXCEPTION;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "toFile(path) requires path");
	path = JS_ToCString(ctx, argv[0]);
	if (!path)
		return JS_EXCEPTION;
	if (!path_allowed(ctx, path, 1)) {
		JS_FreeCString(ctx, path);
		return JS_EXCEPTION;
	}
	rc = vips_image_write_to_file(img->image, path, NULL);
	if (rc != 0) {
		vips_error_clear();
		rc = write_image_stb(path, img->image);
	}
	JS_FreeCString(ctx, path);
	if (rc != 0)
		return JS_ThrowInternalError(ctx, "failed to write image");
	return JS_NewObject(ctx);
}

static JSValue js_sharp_to_buffer(JSContext *ctx, JSValueConst this_val,
				  int argc, JSValueConst *argv)
{
	struct sharp_image *img = sharp_this(ctx, this_val);
	void *buf = NULL;
	size_t len = 0;
	JSValue out;

	(void)argc;
	(void)argv;
	if (!img)
		return JS_EXCEPTION;
	if (vips_image_write_to_buffer(img->image, img->format, &buf, &len,
				       NULL) != 0)
		return JS_ThrowInternalError(ctx, "failed to encode image");
	out = JS_NewArrayBufferCopy(ctx, buf, len);
	g_free(buf);
	return out;
}

static JSValue js_create_canvas(JSContext *ctx, JSValueConst this_val,
				int argc, JSValueConst *argv)
{
	int32_t width;
	int32_t height;

	(void)this_val;
	if (!require_image(ctx))
		return JS_EXCEPTION;
	if (argc < 2)
		return JS_ThrowTypeError(ctx, "createCanvas(width, height)");
	if (JS_ToInt32(ctx, &width, argv[0]) < 0 ||
	    JS_ToInt32(ctx, &height, argv[1]) < 0)
		return JS_EXCEPTION;
	return new_canvas_object(ctx, width, height);
}

static JSValue canvas_get_context(JSContext *ctx, JSValueConst this_val,
				  int argc, JSValueConst *argv)
{
	struct canvas *canvas = JS_GetOpaque2(ctx, this_val, canvas_class_id);
	const char *kind;

	if (!canvas)
		return JS_EXCEPTION;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "getContext(type)");
	kind = JS_ToCString(ctx, argv[0]);
	if (!kind)
		return JS_EXCEPTION;
	if (strcmp(kind, "2d") != 0) {
		JS_FreeCString(ctx, kind);
		return JS_NULL;
	}
	JS_FreeCString(ctx, kind);
	return JS_DupValue(ctx, this_val);
}

static int parse_color(const char *s, double *r, double *g, double *b,
		       double *a)
{
	unsigned int rv;
	unsigned int gv;
	unsigned int bv;

	if (!s || !r || !g || !b || !a)
		return -EINVAL;
	*a = 1.0;
	if (strcmp(s, "white") == 0) {
		*r = 1.0;
		*g = 1.0;
		*b = 1.0;
		return 0;
	}
	if (strcmp(s, "black") == 0) {
		*r = 0.0;
		*g = 0.0;
		*b = 0.0;
		return 0;
	}
	if (strcmp(s, "red") == 0) {
		*r = 1.0;
		*g = 0.0;
		*b = 0.0;
		return 0;
	}
	if (strcmp(s, "green") == 0) {
		*r = 0.0;
		*g = 0.5;
		*b = 0.0;
		return 0;
	}
	if (strcmp(s, "blue") == 0) {
		*r = 0.0;
		*g = 0.0;
		*b = 1.0;
		return 0;
	}
	if (s[0] == '#' && strlen(s) == 7 &&
	    sscanf(s + 1, "%02x%02x%02x", &rv, &gv, &bv) == 3) {
		*r = (double)rv / 255.0;
		*g = (double)gv / 255.0;
		*b = (double)bv / 255.0;
		return 0;
	}
	return -EINVAL;
}

static void canvas_set_source(JSContext *ctx, JSValueConst this_val,
			      const char *prop)
{
	struct canvas *canvas = JS_GetOpaque(this_val, canvas_class_id);
	JSValue val;
	const char *style;
	double r = 0.0;
	double g = 0.0;
	double b = 0.0;
	double a = 1.0;

	if (!canvas)
		return;
	val = JS_GetPropertyStr(ctx, this_val, prop);
	style = JS_ToCString(ctx, val);
	if (style)
		(void)parse_color(style, &r, &g, &b, &a);
	if (style)
		JS_FreeCString(ctx, style);
	JS_FreeValue(ctx, val);
	cairo_set_source_rgba(canvas->cr, r, g, b, a);
}

static JSValue canvas_rect_op(JSContext *ctx, JSValueConst this_val,
			      int argc, JSValueConst *argv, int magic)
{
	struct canvas *canvas = JS_GetOpaque2(ctx, this_val, canvas_class_id);
	double x;
	double y;
	double w;
	double h;

	if (!canvas)
		return JS_EXCEPTION;
	if (argc < 4)
		return JS_ThrowTypeError(ctx, "rectangle requires 4 args");
	if (JS_ToFloat64(ctx, &x, argv[0]) < 0 ||
	    JS_ToFloat64(ctx, &y, argv[1]) < 0 ||
	    JS_ToFloat64(ctx, &w, argv[2]) < 0 ||
	    JS_ToFloat64(ctx, &h, argv[3]) < 0)
		return JS_EXCEPTION;
	cairo_rectangle(canvas->cr, x, y, w, h);
	if (magic == 1) {
		canvas_set_source(ctx, this_val, "fillStyle");
		cairo_fill(canvas->cr);
	} else if (magic == 2) {
		canvas_set_source(ctx, this_val, "strokeStyle");
		cairo_stroke(canvas->cr);
	}
	return JS_UNDEFINED;
}

static JSValue canvas_simple(JSContext *ctx, JSValueConst this_val,
			     int argc, JSValueConst *argv, int magic)
{
	struct canvas *canvas = JS_GetOpaque2(ctx, this_val, canvas_class_id);

	(void)argc;
	(void)argv;
	if (!canvas)
		return JS_EXCEPTION;
	switch (magic) {
	case 1:
		cairo_new_path(canvas->cr);
		break;
	case 2:
		canvas_set_source(ctx, this_val, "fillStyle");
		cairo_fill(canvas->cr);
		break;
	case 3:
		canvas_set_source(ctx, this_val, "strokeStyle");
		cairo_stroke(canvas->cr);
		break;
	case 4:
		cairo_save(canvas->cr);
		break;
	case 5:
		cairo_restore(canvas->cr);
		break;
	default:
		break;
	}
	return JS_UNDEFINED;
}

static JSValue canvas_xy(JSContext *ctx, JSValueConst this_val, int argc,
			 JSValueConst *argv, int magic)
{
	struct canvas *canvas = JS_GetOpaque2(ctx, this_val, canvas_class_id);
	double x;
	double y;

	if (!canvas)
		return JS_EXCEPTION;
	if (argc < 2)
		return JS_ThrowTypeError(ctx, "point requires 2 args");
	if (JS_ToFloat64(ctx, &x, argv[0]) < 0 ||
	    JS_ToFloat64(ctx, &y, argv[1]) < 0)
		return JS_EXCEPTION;
	if (magic == 1)
		cairo_move_to(canvas->cr, x, y);
	else if (magic == 2)
		cairo_line_to(canvas->cr, x, y);
	else
		cairo_translate(canvas->cr, x, y);
	return JS_UNDEFINED;
}

static JSValue canvas_arc(JSContext *ctx, JSValueConst this_val,
			  int argc, JSValueConst *argv)
{
	struct canvas *canvas = JS_GetOpaque2(ctx, this_val, canvas_class_id);
	double x;
	double y;
	double radius;
	double start;
	double end;

	if (!canvas)
		return JS_EXCEPTION;
	if (argc < 5)
		return JS_ThrowTypeError(ctx, "arc(x, y, r, start, end)");
	if (JS_ToFloat64(ctx, &x, argv[0]) < 0 ||
	    JS_ToFloat64(ctx, &y, argv[1]) < 0 ||
	    JS_ToFloat64(ctx, &radius, argv[2]) < 0 ||
	    JS_ToFloat64(ctx, &start, argv[3]) < 0 ||
	    JS_ToFloat64(ctx, &end, argv[4]) < 0)
		return JS_EXCEPTION;
	cairo_arc(canvas->cr, x, y, radius, start, end);
	return JS_UNDEFINED;
}

static JSValue canvas_text(JSContext *ctx, JSValueConst this_val,
			   int argc, JSValueConst *argv, int magic)
{
	struct canvas *canvas = JS_GetOpaque2(ctx, this_val, canvas_class_id);
	const char *text;
	double x;
	double y;

	if (!canvas)
		return JS_EXCEPTION;
	if (argc < 3)
		return JS_ThrowTypeError(ctx, "text requires text, x, y");
	text = JS_ToCString(ctx, argv[0]);
	if (!text)
		return JS_EXCEPTION;
	if (JS_ToFloat64(ctx, &x, argv[1]) < 0 ||
	    JS_ToFloat64(ctx, &y, argv[2]) < 0) {
		JS_FreeCString(ctx, text);
		return JS_EXCEPTION;
	}
	cairo_move_to(canvas->cr, x, y);
	if (magic == 1) {
		canvas_set_source(ctx, this_val, "fillStyle");
		cairo_show_text(canvas->cr, text);
	} else {
		canvas_set_source(ctx, this_val, "strokeStyle");
		cairo_text_path(canvas->cr, text);
		cairo_stroke(canvas->cr);
	}
	JS_FreeCString(ctx, text);
	return JS_UNDEFINED;
}

static JSValue canvas_to_file(JSContext *ctx, JSValueConst this_val,
			      int argc, JSValueConst *argv)
{
	struct canvas *canvas = JS_GetOpaque2(ctx, this_val, canvas_class_id);
	const char *path;
	cairo_status_t status;

	if (!canvas)
		return JS_EXCEPTION;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "toFile(path)");
	path = JS_ToCString(ctx, argv[0]);
	if (!path)
		return JS_EXCEPTION;
	if (!path_allowed(ctx, path, 1)) {
		JS_FreeCString(ctx, path);
		return JS_EXCEPTION;
	}
	cairo_surface_flush(canvas->surface);
	status = cairo_surface_write_to_png(canvas->surface, path);
	JS_FreeCString(ctx, path);
	if (status != CAIRO_STATUS_SUCCESS)
		return JS_ThrowInternalError(ctx, "failed to write canvas");
	return JS_UNDEFINED;
}

static cairo_status_t png_buf_write(void *closure, const unsigned char *data,
				    unsigned int length)
{
	morph_buf_t *buf = closure;

	return morph_buf_append(buf, (const char *)data, length) == 0 ?
		CAIRO_STATUS_SUCCESS : CAIRO_STATUS_WRITE_ERROR;
}

static JSValue canvas_to_buffer(JSContext *ctx, JSValueConst this_val,
				int argc, JSValueConst *argv)
{
	struct canvas *canvas = JS_GetOpaque2(ctx, this_val, canvas_class_id);
	morph_buf_t buf;
	cairo_status_t status;
	JSValue out;

	(void)argc;
	(void)argv;
	if (!canvas)
		return JS_EXCEPTION;
	if (morph_buf_init(&buf, 8192) != 0)
		return JS_ThrowOutOfMemory(ctx);
	cairo_surface_flush(canvas->surface);
	status = cairo_surface_write_to_png_stream(canvas->surface,
						  png_buf_write, &buf);
	if (status != CAIRO_STATUS_SUCCESS) {
		morph_buf_cleanup(&buf);
		return JS_ThrowInternalError(ctx, "failed to encode canvas");
	}
	out = JS_NewArrayBufferCopy(ctx, (const uint8_t *)buf.data, buf.len);
	morph_buf_cleanup(&buf);
	return out;
}

static JSValue js_load_image(JSContext *ctx, JSValueConst this_val,
			     int argc, JSValueConst *argv)
{
	const char *path;
	JSValue out;

	(void)this_val;
	if (!require_image(ctx))
		return JS_EXCEPTION;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "loadImage(path)");
	path = JS_ToCString(ctx, argv[0]);
	if (!path)
		return JS_EXCEPTION;
	if (!path_allowed(ctx, path, 0)) {
		JS_FreeCString(ctx, path);
		return JS_EXCEPTION;
	}
	out = js_sharp_call(ctx, JS_UNDEFINED, 1, argv);
	JS_FreeCString(ctx, path);
	return out;
}

static int read_u32_leb(const uint8_t *buf, size_t len, size_t *pos,
			uint32_t *out)
{
	uint32_t value = 0;
	int shift = 0;

	while (*pos < len && shift < 35) {
		uint8_t byte = buf[(*pos)++];
		value |= (uint32_t)(byte & 0x7f) << shift;
		if ((byte & 0x80) == 0) {
			*out = value;
			return 0;
		}
		shift += 7;
	}
	return -EINVAL;
}

static int parse_wasm_exports(struct wasm_module *module)
{
	const uint8_t *buf = module->bytes;
	size_t pos = 8;
	uint32_t count;

	if (module->len < 8 || memcmp(buf, "\0asm", 4) != 0)
		return -EINVAL;
	while (pos < module->len) {
		uint8_t id;
		uint32_t size;
		size_t end;

		id = buf[pos++];
		if (read_u32_leb(buf, module->len, &pos, &size) < 0)
			return -EINVAL;
		end = pos + size;
		if (end > module->len)
			return -EINVAL;
		if (id != 7) {
			pos = end;
			continue;
		}
		if (read_u32_leb(buf, end, &pos, &count) < 0)
			return -EINVAL;
		for (uint32_t i = 0; i < count && pos < end; i++) {
			uint32_t name_len;
			uint8_t kind;
			uint32_t index;

			if (read_u32_leb(buf, end, &pos, &name_len) < 0 ||
			    pos + name_len > end)
				return -EINVAL;
			if (name_len > 0 && module->export_count < WASM_MAX_EXPORTS) {
				size_t keep = name_len;
				if (keep >= sizeof(module->exports[0]))
					keep = sizeof(module->exports[0]) - 1;
				memcpy(module->exports[module->export_count],
				       buf + pos, keep);
				module->exports[module->export_count][keep] = '\0';
			}
			pos += name_len;
			if (pos >= end)
				return -EINVAL;
			kind = buf[pos++];
			if (read_u32_leb(buf, end, &pos, &index) < 0)
				return -EINVAL;
			(void)index;
			if (kind == 0 && module->export_count < WASM_MAX_EXPORTS)
				module->export_count++;
			else if (kind == 2)
				module->has_memory = 1;
		}
		return 0;
	}
	return 0;
}

static JSValue new_wasm_module(JSContext *ctx, const uint8_t *bytes,
			       size_t len)
{
	struct wasm_module *module;
	JSValue obj;

	module = calloc(1, sizeof(*module));
	if (!module)
		return JS_ThrowOutOfMemory(ctx);
	module->bytes = malloc(len);
	if (!module->bytes) {
		free(module);
		return JS_ThrowOutOfMemory(ctx);
	}
	memcpy(module->bytes, bytes, len);
	module->len = len;
	if (parse_wasm_exports(module) < 0) {
		free(module->bytes);
		free(module);
		return JS_ThrowTypeError(ctx, "invalid wasm module");
	}
	obj = JS_NewObjectClass(ctx, wasm_module_class_id);
	if (JS_IsException(obj)) {
		free(module->bytes);
		free(module);
		return obj;
	}
	JS_SetOpaque(obj, module);
	return obj;
}

static JSValue js_wasm_module_ctor(JSContext *ctx, JSValueConst new_target,
				   int argc, JSValueConst *argv)
{
	uint8_t *bytes;
	size_t len;

	(void)new_target;
	if (!require_wasm(ctx))
		return JS_EXCEPTION;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "WebAssembly.Module(bytes)");
	bytes = JS_GetArrayBuffer(ctx, &len, argv[0]);
	if (!bytes)
		return JS_ThrowTypeError(ctx, "wasm bytes must be ArrayBuffer");
	return new_wasm_module(ctx, bytes, len);
}

static void array_buffer_free(JSRuntime *rt, void *opaque, void *ptr)
{
	(void)rt;
	(void)opaque;
	free(ptr);
}

static JSValue wasm_export_call(JSContext *ctx, JSValueConst this_val,
				int argc, JSValueConst *argv, int magic,
				JSValue *func_data)
{
	struct wasm_instance *inst;
	IM3Function fn;
	const char *name;
	const char *args[16];
	char arg_storage[16][64];
	uint32_t retc;
	uint64_t ret64 = 0;
	const void *rets[1];
	M3Result res;

	(void)this_val;
	inst = JS_GetOpaque2(ctx, func_data[0], wasm_instance_class_id);
	name = JS_ToCString(ctx, func_data[1]);
	if (!inst || !name)
		return JS_EXCEPTION;
	if (argc > 16) {
		JS_FreeCString(ctx, name);
		return JS_ThrowRangeError(ctx, "too many wasm arguments");
	}
	for (int i = 0; i < argc; i++) {
		double value = 0.0;
		(void)JS_ToFloat64(ctx, &value, argv[i]);
		snprintf(arg_storage[i], sizeof(arg_storage[i]), "%.17g", value);
		args[i] = arg_storage[i];
	}
	res = m3_FindFunction(&fn, inst->runtime, name);
	if (res) {
		JS_FreeCString(ctx, name);
		return JS_ThrowInternalError(ctx, "wasm export not found");
	}
	res = m3_CallArgv(fn, (uint32_t)argc, args);
	if (res) {
		JS_FreeCString(ctx, name);
		return JS_ThrowInternalError(ctx, "wasm call failed: %s", res);
	}
	retc = m3_GetRetCount(fn);
	if (retc == 0) {
		JS_FreeCString(ctx, name);
		return JS_UNDEFINED;
	}
	rets[0] = &ret64;
	res = m3_GetResults(fn, 1, rets);
	JS_FreeCString(ctx, name);
	if (res)
		return JS_ThrowInternalError(ctx, "wasm result failed");
	(void)magic;
	return JS_NewInt64(ctx, (int64_t)ret64);
}

static JSValue build_wasm_instance(JSContext *ctx, struct wasm_module *module)
{
	struct wasm_instance *inst;
	IM3Module m3_module = NULL;
	M3Result res;
	JSValue obj;
	JSValue exports;

	inst = calloc(1, sizeof(*inst));
	if (!inst)
		return JS_ThrowOutOfMemory(ctx);
	inst->bytes = malloc(module->len);
	if (!inst->bytes)
		goto oom;
	memcpy(inst->bytes, module->bytes, module->len);
	inst->len = module->len;
	memcpy(inst->exports, module->exports, sizeof(inst->exports));
	inst->export_count = module->export_count;
	inst->env = m3_NewEnvironment();
	if (!inst->env)
		goto oom;
	inst->runtime = m3_NewRuntime(inst->env, WASM_STACK_SIZE, NULL);
	if (!inst->runtime)
		goto oom;
	res = m3_ParseModule(inst->env, &m3_module, inst->bytes,
			     (uint32_t)inst->len);
	if (res)
		goto wasm_fail;
	res = m3_LoadModule(inst->runtime, m3_module);
	m3_module = NULL;
	if (res)
		goto wasm_fail;
	obj = JS_NewObjectClass(ctx, wasm_instance_class_id);
	if (JS_IsException(obj))
		goto oom;
	JS_SetOpaque(obj, inst);
	exports = JS_NewObject(ctx);
	for (int i = 0; i < inst->export_count; i++) {
		JSValue data[2];
		JSValue fn;

		data[0] = JS_DupValue(ctx, obj);
		data[1] = JS_NewString(ctx, inst->exports[i]);
		fn = JS_NewCFunctionData(ctx, wasm_export_call, 0, 0, 2, data);
		JS_FreeValue(ctx, data[0]);
		JS_FreeValue(ctx, data[1]);
		JS_SetPropertyStr(ctx, exports, inst->exports[i], fn);
	}
	JS_SetPropertyStr(ctx, obj, "exports", exports);
	return obj;
wasm_fail:
	if (m3_module)
		m3_FreeModule(m3_module);
	if (inst->runtime)
		m3_FreeRuntime(inst->runtime);
	if (inst->env)
		m3_FreeEnvironment(inst->env);
	free(inst->bytes);
	free(inst);
	return JS_ThrowTypeError(ctx, "failed to instantiate wasm: %s", res);
oom:
	if (m3_module)
		m3_FreeModule(m3_module);
	if (inst) {
		if (inst->runtime)
			m3_FreeRuntime(inst->runtime);
		if (inst->env)
			m3_FreeEnvironment(inst->env);
		free(inst->bytes);
		free(inst);
	}
	return JS_ThrowOutOfMemory(ctx);
}

static JSValue js_wasm_instance_ctor(JSContext *ctx, JSValueConst new_target,
				     int argc, JSValueConst *argv)
{
	struct wasm_module *module;

	(void)new_target;
	if (!require_wasm(ctx))
		return JS_EXCEPTION;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "WebAssembly.Instance(module)");
	module = JS_GetOpaque2(ctx, argv[0], wasm_module_class_id);
	if (!module)
		return JS_EXCEPTION;
	return build_wasm_instance(ctx, module);
}

static JSValue js_wasm_compile(JSContext *ctx, JSValueConst this_val,
			       int argc, JSValueConst *argv)
{
	(void)this_val;
	return js_wasm_module_ctor(ctx, JS_UNDEFINED, argc, argv);
}

static JSValue js_wasm_instantiate(JSContext *ctx, JSValueConst this_val,
				   int argc, JSValueConst *argv)
{
	struct wasm_module *module;
	JSValue module_obj = JS_UNDEFINED;
	JSValue instance;
	JSValue out;

	(void)this_val;
	if (!require_wasm(ctx))
		return JS_EXCEPTION;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "WebAssembly.instantiate(bytes)");
	module = JS_GetOpaque(argv[0], wasm_module_class_id);
	if (module) {
		instance = build_wasm_instance(ctx, module);
		return instance;
	}
	module_obj = js_wasm_module_ctor(ctx, JS_UNDEFINED, argc, argv);
	if (JS_IsException(module_obj))
		return module_obj;
	module = JS_GetOpaque2(ctx, module_obj, wasm_module_class_id);
	if (!module) {
		JS_FreeValue(ctx, module_obj);
		return JS_EXCEPTION;
	}
	instance = build_wasm_instance(ctx, module);
	if (JS_IsException(instance)) {
		JS_FreeValue(ctx, module_obj);
		return instance;
	}
	out = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, out, "module", module_obj);
	JS_SetPropertyStr(ctx, out, "instance", instance);
	return out;
}

static JSValue js_wasm_memory_ctor(JSContext *ctx, JSValueConst new_target,
				   int argc, JSValueConst *argv)
{
	JSValue obj;
	JSValue desc;
	JSValue initial_val;
	int32_t initial = 1;
	size_t len;
	uint8_t *zero;

	(void)new_target;
	if (!require_wasm(ctx))
		return JS_EXCEPTION;
	if (argc > 0 && JS_IsObject(argv[0])) {
		desc = argv[0];
		initial_val = JS_GetPropertyStr(ctx, desc, "initial");
		(void)JS_ToInt32(ctx, &initial, initial_val);
		JS_FreeValue(ctx, initial_val);
	}
	if (initial <= 0 || initial > 256)
		return JS_ThrowRangeError(ctx, "invalid memory size");
	len = (size_t)initial * 65536;
	zero = calloc(1, len);
	if (!zero)
		return JS_ThrowOutOfMemory(ctx);
	obj = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, obj, "buffer",
			  JS_NewArrayBuffer(ctx, zero, len, array_buffer_free,
					    NULL, 0));
	return obj;
}

static JSValue js_require(JSContext *ctx, JSValueConst this_val,
			  int argc, JSValueConst *argv);

static const JSCFunctionListEntry sharp_proto_funcs[] = {
	JS_CFUNC_DEF("metadata", 0, js_sharp_metadata),
	JS_CFUNC_DEF("resize", 2, js_sharp_resize),
	JS_CFUNC_DEF("extract", 1, js_sharp_extract),
	JS_CFUNC_DEF("extend", 1, js_sharp_extend),
	JS_CFUNC_DEF("rotate", 1, js_sharp_rotate),
	JS_CFUNC_DEF("blur", 1, js_sharp_blur),
	JS_CFUNC_DEF("sharpen", 0, js_sharp_sharpen),
	JS_CFUNC_DEF("grayscale", 0, js_sharp_grayscale),
	JS_CFUNC_DEF("greyscale", 0, js_sharp_grayscale),
	JS_CFUNC_DEF("flatten", 0, js_sharp_flatten),
	JS_CFUNC_DEF("composite", 1, js_sharp_composite),
	JS_CFUNC_MAGIC_DEF("png", 0, js_sharp_format, 0),
	JS_CFUNC_MAGIC_DEF("jpeg", 0, js_sharp_format, 1),
	JS_CFUNC_MAGIC_DEF("jpg", 0, js_sharp_format, 1),
	JS_CFUNC_MAGIC_DEF("webp", 0, js_sharp_format, 2),
	JS_CFUNC_DEF("toFile", 1, js_sharp_to_file),
	JS_CFUNC_DEF("toBuffer", 0, js_sharp_to_buffer),
};

static const JSCFunctionListEntry canvas_proto_funcs[] = {
	JS_CFUNC_DEF("getContext", 1, canvas_get_context),
	JS_CFUNC_MAGIC_DEF("fillRect", 4, canvas_rect_op, 1),
	JS_CFUNC_MAGIC_DEF("strokeRect", 4, canvas_rect_op, 2),
	JS_CFUNC_MAGIC_DEF("rect", 4, canvas_rect_op, 0),
	JS_CFUNC_MAGIC_DEF("beginPath", 0, canvas_simple, 1),
	JS_CFUNC_MAGIC_DEF("fill", 0, canvas_simple, 2),
	JS_CFUNC_MAGIC_DEF("stroke", 0, canvas_simple, 3),
	JS_CFUNC_MAGIC_DEF("save", 0, canvas_simple, 4),
	JS_CFUNC_MAGIC_DEF("restore", 0, canvas_simple, 5),
	JS_CFUNC_MAGIC_DEF("moveTo", 2, canvas_xy, 1),
	JS_CFUNC_MAGIC_DEF("lineTo", 2, canvas_xy, 2),
	JS_CFUNC_MAGIC_DEF("translate", 2, canvas_xy, 3),
	JS_CFUNC_DEF("arc", 5, canvas_arc),
	JS_CFUNC_MAGIC_DEF("fillText", 3, canvas_text, 1),
	JS_CFUNC_MAGIC_DEF("strokeText", 3, canvas_text, 2),
	JS_CFUNC_DEF("toFile", 1, canvas_to_file),
	JS_CFUNC_DEF("toBuffer", 1, canvas_to_buffer),
};

static void install_classes(JSContext *ctx)
{
	JSRuntime *rt = JS_GetRuntime(ctx);
	JSValue proto;

	if (!classes_inited) {
		JS_NewClassID(&sharp_class_id);
		JS_NewClassID(&canvas_class_id);
		JS_NewClassID(&image_data_class_id);
		JS_NewClassID(&wasm_module_class_id);
		JS_NewClassID(&wasm_instance_class_id);
		classes_inited = 1;
	}
	{
		JSClassDef def = {"Sharp", .finalizer = sharp_finalizer};
		JS_NewClass(rt, sharp_class_id, &def);
	}
	{
		JSClassDef def = {"Canvas", .finalizer = canvas_finalizer};
		JS_NewClass(rt, canvas_class_id, &def);
	}
	{
		JSClassDef def = {"ImageData", .finalizer = image_data_finalizer};
		JS_NewClass(rt, image_data_class_id, &def);
	}
	{
		JSClassDef def = {"WasmModule", .finalizer = wasm_module_finalizer};
		JS_NewClass(rt, wasm_module_class_id, &def);
	}
	{
		JSClassDef def = {"WasmInstance",
				  .finalizer = wasm_instance_finalizer};
		JS_NewClass(rt, wasm_instance_class_id, &def);
	}
	proto = JS_NewObject(ctx);
	JS_SetPropertyFunctionList(ctx, proto, sharp_proto_funcs,
				   sizeof(sharp_proto_funcs) /
				   sizeof(sharp_proto_funcs[0]));
	JS_SetClassProto(ctx, sharp_class_id, proto);
	proto = JS_NewObject(ctx);
	JS_SetPropertyFunctionList(ctx, proto, canvas_proto_funcs,
				   sizeof(canvas_proto_funcs) /
				   sizeof(canvas_proto_funcs[0]));
	JS_SetClassProto(ctx, canvas_class_id, proto);
}

static JSValue js_require(JSContext *ctx, JSValueConst this_val,
			  int argc, JSValueConst *argv)
{
	const char *name;
	JSValue out;

	(void)this_val;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "require(name)");
	name = JS_ToCString(ctx, argv[0]);
	if (!name)
		return JS_EXCEPTION;
	if (strcmp(name, "sharp") == 0) {
		out = JS_NewCFunction(ctx, js_sharp_call, "sharp", 1);
	} else if (strcmp(name, "canvas") == 0) {
		out = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, out, "createCanvas",
				  JS_NewCFunction(ctx, js_create_canvas,
						  "createCanvas", 2));
		JS_SetPropertyStr(ctx, out, "loadImage",
				  JS_NewCFunction(ctx, js_load_image,
						  "loadImage", 1));
	} else {
		JS_FreeCString(ctx, name);
		return JS_ThrowTypeError(ctx,
					 "unsupported require() module");
	}
	JS_FreeCString(ctx, name);
	return out;
}

int js_media_init(void)
{
	if (VIPS_INIT("morph-js-runner"))
		return -EINVAL;
	return 0;
}

void js_media_shutdown(void)
{
	vips_shutdown();
}

void install_media_api(JSContext *ctx)
{
	JSValue global;
	JSValue wasm;
	JSValue module_ctor;
	JSValue instance_ctor;
	JSValue memory_ctor;

	install_classes(ctx);
	global = JS_GetGlobalObject(ctx);
	JS_SetPropertyStr(ctx, global, "require",
			  JS_NewCFunction(ctx, js_require, "require", 1));
	JS_SetPropertyStr(ctx, global, "sharp",
			  JS_NewCFunction(ctx, js_sharp_call, "sharp", 1));
	JS_SetPropertyStr(ctx, global, "createCanvas",
			  JS_NewCFunction(ctx, js_create_canvas,
					  "createCanvas", 2));
	JS_SetPropertyStr(ctx, global, "loadImage",
			  JS_NewCFunction(ctx, js_load_image, "loadImage", 1));
	wasm = JS_NewObject(ctx);
	module_ctor = JS_NewCFunction2(ctx, js_wasm_module_ctor, "Module", 1,
				       JS_CFUNC_constructor, 0);
	instance_ctor = JS_NewCFunction2(ctx, js_wasm_instance_ctor,
					 "Instance", 2,
					 JS_CFUNC_constructor, 0);
	memory_ctor = JS_NewCFunction2(ctx, js_wasm_memory_ctor, "Memory", 1,
				       JS_CFUNC_constructor, 0);
	JS_SetPropertyStr(ctx, wasm, "Module", module_ctor);
	JS_SetPropertyStr(ctx, wasm, "Instance", instance_ctor);
	JS_SetPropertyStr(ctx, wasm, "Memory", memory_ctor);
	JS_SetPropertyStr(ctx, wasm, "compile",
			  JS_NewCFunction(ctx, js_wasm_compile, "compile", 1));
	JS_SetPropertyStr(ctx, wasm, "instantiate",
			  JS_NewCFunction(ctx, js_wasm_instantiate,
					  "instantiate", 2));
	JS_SetPropertyStr(ctx, global, "WebAssembly", wasm);
	JS_FreeValue(ctx, global);
}
