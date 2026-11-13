#include "js_image.h"
#include "nb/err.h"
#include "js_values.h"
#include "js_canvas.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/file.h"
#include "nb/cleanup.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pango/pangocairo.h>
#ifdef __ANDROID__
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image.h"
#include "stb_image_write.h"

static void sharp_finalizer(JSRuntime *rt, JSValue val);
static JSValue new_sharp_object(JSContext *ctx, VipsImage *image, const char *format);
static JSValue throw_vips_error(JSContext *ctx, const char *prefix);
static VipsImage *sharp_load_input(JSContext *ctx, JSValueConst value);
static VipsImage *image_from_stb_pixels(JSContext *ctx, unsigned char *pixels, int width,
					int height);
static VipsImage *image_set_interpretation(VipsImage *image);
static int write_image_stb(const char *path, VipsImage *image);
static void stb_buf_write(void *context, void *data, int size);
static int write_image_stb_buffer(const char *format, VipsImage *image, void **out,
				  size_t *out_len);
static int parse_js_color(JSContext *ctx, JSValueConst value, double color[4]);
static unsigned char clamp_color(double value);
static VipsImage *sharp_create_input(JSContext *ctx, JSValueConst create);
static struct sharp_image *sharp_this(JSContext *ctx, JSValueConst val);
static JSValue sharp_replace(JSContext *ctx, JSValueConst this_val, VipsImage *next);
static JSValue js_sharp_metadata(JSContext *ctx, JSValueConst this_val, int argc,
				 JSValueConst *argv);
static JSValue js_sharp_resize(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_sharp_extract(JSContext *ctx, JSValueConst this_val, int argc,
				JSValueConst *argv);
static JSValue js_sharp_extend(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_sharp_rotate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_sharp_blur(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_sharp_sharpen(JSContext *ctx, JSValueConst this_val, int argc,
				JSValueConst *argv);
static JSValue js_sharp_grayscale(JSContext *ctx, JSValueConst this_val, int argc,
				  JSValueConst *argv);
static JSValue js_sharp_flatten(JSContext *ctx, JSValueConst this_val, int argc,
				JSValueConst *argv);
static JSValue js_sharp_composite(JSContext *ctx, JSValueConst this_val, int argc,
				  JSValueConst *argv);
static JSValue js_sharp_format(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
			       int magic);
static JSValue js_sharp_to_file(JSContext *ctx, JSValueConst this_val, int argc,
				JSValueConst *argv);
static JSValue js_sharp_to_buffer(JSContext *ctx, JSValueConst this_val, int argc,
				  JSValueConst *argv);
static JSValue js_image_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_image_create(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_image_metadata_fn(JSContext *ctx, JSValueConst this_val, int argc,
				    JSValueConst *argv);
static JSValue image_transform_save(JSContext *ctx, JSValueConst options,
				    JSValue (*fn)(JSContext *, JSValueConst, int, JSValueConst *),
				    int argc, JSValueConst *argv);
static JSValue js_image_resize_fn(JSContext *ctx, JSValueConst this_val, int argc,
				  JSValueConst *argv);
static JSValue js_image_crop_fn(JSContext *ctx, JSValueConst this_val, int argc,
				JSValueConst *argv);
static JSValue js_image_extend_fn(JSContext *ctx, JSValueConst this_val, int argc,
				  JSValueConst *argv);
static JSValue js_image_rotate_fn(JSContext *ctx, JSValueConst this_val, int argc,
				  JSValueConst *argv);
static JSValue js_image_compose_fn(JSContext *ctx, JSValueConst this_val, int argc,
				   JSValueConst *argv);
static JSValue js_image_convert_fn(JSContext *ctx, JSValueConst this_val, int argc,
				   JSValueConst *argv);
static JSValue js_image_frame_fn(JSContext *ctx, JSValueConst this_val, int argc,
				 JSValueConst *argv);

int js_image_require(JSContext *ctx)
{
	if (!runner_has_cap("image")) {
		runner_throw_cap(ctx, "image");
		return 0;
	}
	return 1;
}

static void sharp_finalizer(JSRuntime *rt, JSValue val)
{
	struct sharp_image *img = JS_GetOpaque(val, JS_GetClassID(val));

	(void)rt;
	if (!img)
		return;
	if (img->image)
		g_object_unref(img->image);
	free(img);
}

static JSValue new_sharp_object(JSContext *ctx, VipsImage *image, const char *format)
{
	struct sharp_image *img;
	JSValue obj;

	img = calloc(1, sizeof(*img));
	if (!img) {
		if (image)
			g_object_unref(image);
		return JS_ThrowOutOfMemory(ctx);
	}
	img->image = image;
	strncpy(img->format, format ? format : ".png", sizeof(img->format) - 1);
	obj = JS_NewObjectClass(ctx, js_values_state_borrow(ctx)->sharp_class_id);
	if (JS_IsException(obj)) {
		if (img->image)
			g_object_unref(img->image);
		free(img);
		return obj;
	}
	JS_SetOpaque(obj, img);
	JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, vips_image_get_width(image)));
	JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, vips_image_get_height(image)));
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

/* libvips decodes lazily and borrows encoded bytes until the image closes. */
static VipsImage *sharp_image_from_bytes_copy(const uint8_t *data, size_t len)
{
	uint8_t *copy = malloc(len ? len : 1);
	VipsImage *image;

	if (!copy)
		return NULL;
	if (len)
		memcpy(copy, data, len);
	image = vips_image_new_from_buffer(copy, len, "", "access", VIPS_ACCESS_SEQUENTIAL, NULL);
	if (!image) {
		free(copy);
		return NULL;
	}
	g_object_set_data_full(G_OBJECT(image), "morph-input-buffer", copy, free);
	return image;
}

static VipsImage *sharp_load_input(JSContext *ctx, JSValueConst value)
{
	const char *path;
	char *resolved;
	struct js_buffer_view view NB_CLEANUP(js_buffer_view_cleanup) = {0};
	const uint8_t *bytes;
	size_t len;
	VipsImage *image;
	int rc;

	if (JS_IsString(value)) {
		path = JS_ToCString(ctx, value);
		if (!path)
			return NULL;
		resolved = runner_authorize_path_copy(ctx, path, 0);
		JS_FreeCString(ctx, path);
		if (!resolved)
			return NULL;
#ifdef __ANDROID__
		{
			int width = 0;
			int height = 0;
			int channels = 0;
			unsigned char *pixels;

			pixels = stbi_load(resolved, &width, &height, &channels, 4);
			image = image_from_stb_pixels(ctx, pixels, width, height);
			if (image) {
				free(resolved);
				return image;
			}
		}
#endif
		image = vips_image_new_from_file(resolved, "access", VIPS_ACCESS_SEQUENTIAL, NULL);
		if (!image) {
			int width = 0;
			int height = 0;
			int channels = 0;
			unsigned char *pixels;

			vips_error_clear();
			pixels = stbi_load(resolved, &width, &height, &channels, 4);
			image = image_from_stb_pixels(ctx, pixels, width, height);
		}
		free(resolved);
		if (!image)
			(void)throw_vips_error(ctx, "failed to load image");
		return image;
	}
	rc = js_values_buffer_borrow(ctx, value, &view);
	if (rc < 0) {
		if (rc == NB_ERR_UNSUPPORTED)
			JS_ThrowTypeError(ctx, "sharp(input) requires a path string, "
					       "ArrayBuffer, or typed array");
		return NULL;
	}
	bytes = view.data;
	len = view.length;
#ifdef __ANDROID__
	if (len <= (size_t)INT_MAX) {
		int width = 0;
		int height = 0;
		int channels = 0;
		unsigned char *pixels;

		pixels = stbi_load_from_memory(bytes, (int)len, &width, &height, &channels, 4);
		image = image_from_stb_pixels(ctx, pixels, width, height);
		if (image)
			return image;
	}
#endif
	image = sharp_image_from_bytes_copy(bytes, len);
	if (!image && len <= (size_t)INT_MAX) {
		int width = 0;
		int height = 0;
		int channels = 0;
		unsigned char *pixels;

		vips_error_clear();
		pixels = stbi_load_from_memory(bytes, (int)len, &width, &height, &channels, 4);
		image = image_from_stb_pixels(ctx, pixels, width, height);
	}
	if (!image)
		(void)throw_vips_error(ctx, "failed to load image buffer");
	return image;
}

static VipsImage *image_from_stb_pixels(JSContext *ctx, unsigned char *pixels, int width,
					int height)
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
	image = vips_image_new_from_memory_copy(pixels, len, width, height, 4, VIPS_FORMAT_UCHAR);
	stbi_image_free(pixels);
	if (!image)
		(void)throw_vips_error(ctx, "failed to create image from pixels");
	image = image_set_interpretation(image);
	return image;
}

static VipsImage *image_set_interpretation(VipsImage *image)
{
	VipsImage *out = NULL;
	VipsInterpretation interpretation;
	int bands;

	if (!image)
		return NULL;
	bands = vips_image_get_bands(image);
	interpretation = bands == 1 ? VIPS_INTERPRETATION_B_W : VIPS_INTERPRETATION_sRGB;
	if (vips_copy(image, &out, "interpretation", interpretation, NULL) != 0) {
		vips_error_clear();
		return image;
	}
	g_object_unref(image);
	return out;
}

int js_image_path_ext_is(const char *path, const char *ext)
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
		return NB_ERR_INVAL;
	width = vips_image_get_width(image);
	height = vips_image_get_height(image);
	bands = vips_image_get_bands(image);
	if (width <= 0 || height <= 0 || bands <= 0 || bands > 4)
		return NB_ERR_INVAL;
	mem = vips_image_write_to_memory(image, &len);
	if (!mem)
		return NB_ERR_IO;
	if (js_image_path_ext_is(path, ".jpg") || js_image_path_ext_is(path, ".jpeg")) {
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
				ok = stbi_write_jpg(path, width, height, 3, rgb, 90);
				free(rgb);
			}
		} else {
			ok = stbi_write_jpg(path, width, height, bands, mem, 90);
		}
	} else {
		ok = stbi_write_png(path, width, height, bands, mem, width * bands);
	}
	g_free(mem);
	return ok ? 0 : NB_ERR_IO;
}

static void stb_buf_write(void *context, void *data, int size)
{
	nb_buf *buf = context;

	if (!buf || !data || size <= 0)
		return;
	(void)nb_buf_append(buf, data, (size_t)size);
}

static int write_image_stb_buffer(const char *format, VipsImage *image, void **out, size_t *out_len)
{
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
	void *mem;
	size_t len;
	int width;
	int height;
	int bands;
	int ok = 0;

	if (!format || !image || !out || !out_len)
		return NB_ERR_INVAL;
	*out = NULL;
	*out_len = 0;
	width = vips_image_get_width(image);
	height = vips_image_get_height(image);
	bands = vips_image_get_bands(image);
	if (width <= 0 || height <= 0 || bands <= 0 || bands > 4)
		return NB_ERR_INVAL;
	mem = vips_image_write_to_memory(image, &len);
	if (!mem)
		return NB_ERR_IO;
	if ((nb_buf_init(&buf), nb_buf_reserve(&buf, 8192) ? 0 : NB_ERR_NOMEM) != 0) {
		g_free(mem);
		return NB_ERR_NOMEM;
	}
	if (strcmp(format, ".jpg") == 0 || strcmp(format, ".jpeg") == 0) {
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
				ok = stbi_write_jpg_to_func(stb_buf_write, &buf, width, height, 3,
							    rgb, 90);
				free(rgb);
			}
		} else {
			ok = stbi_write_jpg_to_func(stb_buf_write, &buf, width, height, bands, mem,
						    90);
		}
	} else if (strcmp(format, ".png") == 0) {
		ok = stbi_write_png_to_func(stb_buf_write, &buf, width, height, bands, mem,
					    width * bands);
	}
	g_free(mem);
	if (!ok) {
		nb_buf_cleanup(&buf);
		return NB_ERR_IO;
	}
	if (!nb_buf_ok(&buf))
		return nb_buf_status(&buf);
	size_t size = nb_buf_len(&buf);
	unsigned char *encoded NB_CLEANUP(nb_ucharp_cleanup) = malloc(size + 1);
	if (!encoded)
		return NB_ERR_NOMEM;
	memcpy(encoded, nb_buf_cstr(&buf), size);
	encoded[size] = '\0';
	*out = encoded;
	encoded = NULL;
	*out_len = size;
	return 0;
}

static int parse_js_color(JSContext *ctx, JSValueConst value, double color[4])
{
	const char *s;
	unsigned int rv;
	unsigned int gv;
	unsigned int bv;

	if (!color)
		return NB_ERR_INVAL;
	color[0] = 0.0;
	color[1] = 0.0;
	color[2] = 0.0;
	color[3] = 255.0;
	if (JS_IsString(value)) {
		s = JS_ToCString(ctx, value);
		if (!s)
			return NB_ERR_INVAL;
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
		JS_ThrowTypeError(ctx, "sharp({create}) requires create to be an object with "
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
		(void)JS_ThrowRangeError(ctx, "sharp({create}) invalid size: width and height must "
					      "be positive integers up to 32768");
		return NULL;
	}
	if (channels < 1 || channels > 4) {
		(void)JS_ThrowRangeError(ctx, "sharp({create}) invalid channels: expected 1, 2, 3, "
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
			data[off] = (unsigned char)(((int)r + (int)g + (int)b) / 3);
		} else if (channels == 2) {
			data[off] = (unsigned char)(((int)r + (int)g + (int)b) / 3);
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
	image = vips_image_new_from_memory_copy(data, len, width, height, channels,
						VIPS_FORMAT_UCHAR);
	free(data);
	if (!image)
		(void)throw_vips_error(ctx, "failed to create image");
	image = image_set_interpretation(image);
	return image;
}

JSValue js_image_open_value(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	VipsImage *image;
	JSValue create = JS_UNDEFINED;

	(void)this_val;
	if (!js_image_require(ctx))
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
	struct sharp_image *img =
		JS_GetOpaque2(ctx, val, js_values_state_borrow(ctx)->sharp_class_id);

	return img;
}

static JSValue sharp_replace(JSContext *ctx, JSValueConst this_val, VipsImage *next)
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

static JSValue js_sharp_metadata(JSContext *ctx, JSValueConst this_val, int argc,
				 JSValueConst *argv)
{
	struct sharp_image *img = sharp_this(ctx, this_val);
	JSValue obj;

	(void)argc;
	(void)argv;
	if (!img)
		return JS_EXCEPTION;
	obj = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, vips_image_get_width(img->image)));
	JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, vips_image_get_height(img->image)));
	JS_SetPropertyStr(ctx, obj, "channels", JS_NewInt32(ctx, vips_image_get_bands(img->image)));
	JS_SetPropertyStr(ctx, obj, "format", JS_NewString(ctx, img->format));
	return obj;
}

static JSValue js_sharp_resize(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
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
		scale = fmin((double)width / (double)ow, (double)height / (double)oh);
	if (scale <= 0.0)
		return JS_ThrowRangeError(ctx, "invalid resize scale");
	if (vips_resize(img->image, &next, scale, NULL) != 0)
		next = NULL;
	return sharp_replace(ctx, this_val, next);
}

static JSValue js_sharp_extract(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
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
	if (vips_extract_area(img->image, &next, left, top, width, height, NULL) != 0)
		next = NULL;
	return sharp_replace(ctx, this_val, next);
}

static JSValue js_sharp_extend(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
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
	int bands;
	int background_len;
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
	bands = vips_image_get_bands(img->image);
	if (bands >= 4)
		background_len = 4;
	else
		background_len = bands == 1 ? 1 : 3;
	v = JS_GetPropertyStr(ctx, argv[0], "background");
	(void)parse_js_color(ctx, v, background);
	JS_FreeValue(ctx, v);
	background_array = vips_array_double_new(background, background_len);
	if (!background_array)
		return JS_ThrowOutOfMemory(ctx);
	if (vips_embed(img->image, &next, left, top, width, height, "extend",
		       VIPS_EXTEND_BACKGROUND, "background", background_array, NULL) != 0) {
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

static JSValue js_sharp_rotate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
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
	else if (vips_similarity(img->image, &next, "angle", raw_angle, NULL) != 0)
		return throw_vips_error(ctx, "failed to rotate image");
	return sharp_replace(ctx, this_val, next);
}

static JSValue js_sharp_blur(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
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

static JSValue js_sharp_sharpen(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
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

static JSValue js_sharp_grayscale(JSContext *ctx, JSValueConst this_val, int argc,
				  JSValueConst *argv)
{
	struct sharp_image *img = sharp_this(ctx, this_val);
	VipsImage *next = NULL;

	(void)argc;
	(void)argv;
	if (!img)
		return JS_EXCEPTION;
	if (vips_colourspace(img->image, &next, VIPS_INTERPRETATION_B_W, NULL) != 0)
		next = NULL;
	return sharp_replace(ctx, this_val, next);
}

static JSValue js_sharp_flatten(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
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

static JSValue js_sharp_composite(JSContext *ctx, JSValueConst this_val, int argc,
				  JSValueConst *argv)
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
			return JS_ThrowTypeError(ctx, "composite overlay must be object");
		}
		input = JS_GetPropertyStr(ctx, item, "input");
		if (JS_IsUndefined(input)) {
			JS_FreeValue(ctx, input);
			JS_FreeValue(ctx, item);
			return JS_ThrowTypeError(ctx, "composite overlay requires input");
		}
		left_val = JS_GetPropertyStr(ctx, item, "left");
		top_val = JS_GetPropertyStr(ctx, item, "top");
		(void)JS_ToInt32(ctx, &left, left_val);
		(void)JS_ToInt32(ctx, &top, top_val);
		JS_FreeValue(ctx, left_val);
		JS_FreeValue(ctx, top_val);
		overlay_sharp = JS_GetOpaque(input, js_values_state_borrow(ctx)->sharp_class_id);
		if (overlay_sharp && overlay_sharp->image)
			overlay = g_object_ref(overlay_sharp->image);
		else
			overlay = sharp_load_input(ctx, input);
		JS_FreeValue(ctx, input);
		JS_FreeValue(ctx, item);
		if (!overlay)
			return JS_EXCEPTION;
		if (vips_composite2(img->image, overlay, &next, VIPS_BLEND_MODE_OVER, "x", left,
				    "y", top, NULL) != 0) {
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

static JSValue js_sharp_format(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
			       int magic)
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

static JSValue js_sharp_to_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	struct sharp_image *img = sharp_this(ctx, this_val);
	const char *path;
	char *resolved;
	int rc;

	if (!img)
		return JS_EXCEPTION;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "toFile(path) requires path");
	path = JS_ToCString(ctx, argv[0]);
	if (!path)
		return JS_EXCEPTION;
	resolved = runner_authorize_path_copy(ctx, path, 1);
	JS_FreeCString(ctx, path);
	if (!resolved)
		return JS_EXCEPTION;
	rc = vips_image_write_to_file(img->image, resolved, NULL);
	if (rc != 0) {
		vips_error_clear();
		rc = write_image_stb(resolved, img->image);
	}
	free(resolved);
	if (rc != 0)
		return JS_ThrowInternalError(ctx, "failed to write image");
	return JS_NewObject(ctx);
}

static JSValue js_sharp_to_buffer(JSContext *ctx, JSValueConst this_val, int argc,
				  JSValueConst *argv)
{
	struct sharp_image *img = sharp_this(ctx, this_val);
	void *buf = NULL;
	size_t len = 0;
	JSValue out;

	(void)argc;
	(void)argv;
	if (!img)
		return JS_EXCEPTION;
	if ((strcmp(img->format, ".png") == 0 || strcmp(img->format, ".jpg") == 0 ||
	     strcmp(img->format, ".jpeg") == 0) &&
	    write_image_stb_buffer(img->format, img->image, &buf, &len) == 0) {
		out = JS_NewArrayBufferCopy(ctx, buf, len);
		free(buf);
		return out;
	}
	if (vips_image_write_to_buffer(img->image, img->format, &buf, &len, NULL) != 0) {
		vips_error_clear();
		return JS_ThrowInternalError(ctx, "failed to encode image");
	}
	out = JS_NewArrayBufferCopy(ctx, buf, len);
	g_free(buf);
	return out;
}

static JSValue js_image_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	JSValue input;
	JSValue out;

	(void)this_val;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "morph.image.open(input)");
	input = js_values_arg_or_prop_retain(ctx, argv[0], "input");
	if (JS_IsUndefined(input))
		return JS_ThrowTypeError(ctx, "morph.image.open requires input");
	out = js_image_open_value(ctx, JS_UNDEFINED, 1, &input);
	JS_FreeValue(ctx, input);
	return out;
}

static JSValue js_image_create(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	JSValue outer;
	JSValue create;
	JSValue out;

	(void)this_val;
	if (argc < 1 || !JS_IsObject(argv[0]))
		return JS_ThrowTypeError(ctx, "morph.image.create(options)");
	outer = JS_NewObject(ctx);
	create = JS_DupValue(ctx, argv[0]);
	JS_SetPropertyStr(ctx, outer, "create", create);
	out = js_image_open_value(ctx, JS_UNDEFINED, 1, &outer);
	JS_FreeValue(ctx, outer);
	return out;
}

static JSValue js_image_metadata_fn(JSContext *ctx, JSValueConst this_val, int argc,
				    JSValueConst *argv)
{
	JSValue image;
	JSValue out;

	(void)this_val;
	image = js_image_open(ctx, JS_UNDEFINED, argc, argv);
	if (JS_IsException(image))
		return image;
	out = js_sharp_metadata(ctx, image, 0, NULL);
	JS_FreeValue(ctx, image);
	return out;
}

static JSValue image_transform_save(JSContext *ctx, JSValueConst options,
				    JSValue (*fn)(JSContext *, JSValueConst, int, JSValueConst *),
				    int argc, JSValueConst *argv)
{
	JSValue image;
	JSValue result;
	JSValue output;
	JSValue out = JS_UNDEFINED;

	image = js_image_open(ctx, JS_UNDEFINED, 1, &options);
	if (JS_IsException(image))
		return image;
	result = fn(ctx, image, argc, argv);
	if (JS_IsException(result)) {
		JS_FreeValue(ctx, image);
		return result;
	}
	JS_FreeValue(ctx, result);
	output = JS_GetPropertyStr(ctx, options, "output");
	if (!JS_IsUndefined(output) && !JS_IsNull(output)) {
		result = js_sharp_to_file(ctx, image, 1, &output);
		JS_FreeValue(ctx, output);
		if (JS_IsException(result)) {
			JS_FreeValue(ctx, image);
			return result;
		}
		JS_FreeValue(ctx, result);
		out = js_sharp_metadata(ctx, image, 0, NULL);
	} else {
		out = JS_DupValue(ctx, image);
		JS_FreeValue(ctx, output);
	}
	JS_FreeValue(ctx, image);
	return out;
}

static JSValue js_image_resize_fn(JSContext *ctx, JSValueConst this_val, int argc,
				  JSValueConst *argv)
{
	JSValue args[2] = {JS_UNDEFINED, JS_UNDEFINED};
	JSValue out;

	(void)this_val;
	(void)argc;
	if (argc < 1 || !JS_IsObject(argv[0]))
		return JS_ThrowTypeError(ctx, "morph.image.resize(options)");
	args[0] = JS_GetPropertyStr(ctx, argv[0], "width");
	args[1] = JS_GetPropertyStr(ctx, argv[0], "height");
	out = image_transform_save(ctx, argv[0], js_sharp_resize, 2, args);
	JS_FreeValue(ctx, args[0]);
	JS_FreeValue(ctx, args[1]);
	return out;
}

static JSValue js_image_crop_fn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	(void)this_val;
	(void)argc;
	if (argc < 1 || !JS_IsObject(argv[0]))
		return JS_ThrowTypeError(ctx, "morph.image.crop(options)");
	return image_transform_save(ctx, argv[0], js_sharp_extract, 1, argv);
}

static JSValue js_image_extend_fn(JSContext *ctx, JSValueConst this_val, int argc,
				  JSValueConst *argv)
{
	(void)this_val;
	(void)argc;
	if (argc < 1 || !JS_IsObject(argv[0]))
		return JS_ThrowTypeError(ctx, "morph.image.extend(options)");
	return image_transform_save(ctx, argv[0], js_sharp_extend, 1, argv);
}

static JSValue js_image_rotate_fn(JSContext *ctx, JSValueConst this_val, int argc,
				  JSValueConst *argv)
{
	JSValue angle;
	JSValue out;

	(void)this_val;
	(void)argc;
	if (argc < 1 || !JS_IsObject(argv[0]))
		return JS_ThrowTypeError(ctx, "morph.image.rotate(options)");
	angle = JS_GetPropertyStr(ctx, argv[0], "angle");
	out = image_transform_save(ctx, argv[0], js_sharp_rotate, 1, &angle);
	JS_FreeValue(ctx, angle);
	return out;
}

static JSValue js_image_compose_fn(JSContext *ctx, JSValueConst this_val, int argc,
				   JSValueConst *argv)
{
	JSValue overlays;
	JSValue out;

	(void)this_val;
	(void)argc;
	if (argc < 1 || !JS_IsObject(argv[0]))
		return JS_ThrowTypeError(ctx, "morph.image.compose(options)");
	overlays = JS_GetPropertyStr(ctx, argv[0], "overlays");
	out = image_transform_save(ctx, argv[0], js_sharp_composite, 1, &overlays);
	JS_FreeValue(ctx, overlays);
	return out;
}

static JSValue js_image_convert_fn(JSContext *ctx, JSValueConst this_val, int argc,
				   JSValueConst *argv)
{
	JSValue image;
	JSValue output;
	JSValue result;
	JSValue out;

	(void)this_val;
	(void)argc;
	if (argc < 1 || !JS_IsObject(argv[0]))
		return JS_ThrowTypeError(ctx, "morph.image.convert(options)");
	image = js_image_open(ctx, JS_UNDEFINED, 1, argv);
	if (JS_IsException(image))
		return image;
	output = JS_GetPropertyStr(ctx, argv[0], "output");
	result = js_sharp_to_file(ctx, image, 1, &output);
	JS_FreeValue(ctx, output);
	if (JS_IsException(result)) {
		JS_FreeValue(ctx, image);
		return result;
	}
	JS_FreeValue(ctx, result);
	out = js_sharp_metadata(ctx, image, 0, NULL);
	JS_FreeValue(ctx, image);
	return out;
}

static JSValue js_image_frame_fn(JSContext *ctx, JSValueConst this_val, int argc,
				 JSValueConst *argv)
{
	JSValue input_val;
	VipsImage *image = NULL;
	struct sharp_image tmp;
	cairo_surface_t *src = NULL;
	cairo_surface_t *surface = NULL;
	cairo_t *cr = NULL;
	const char *output;
	char *resolved_output;
	const char *style;
	const char *caption;
	int32_t pad = 48;
	int width;
	int height;
	int frame_w;
	int frame_h;
	int rc = 0;
	JSValue out;

	(void)this_val;
	(void)argc;
	if (!js_image_require(ctx))
		return JS_EXCEPTION;
	if (argc < 1 || !JS_IsObject(argv[0]))
		return JS_ThrowTypeError(ctx, "morph.image.frame(options)");
	output = js_values_object_string_prop_copy(ctx, argv[0], "output");
	if (!output)
		return JS_ThrowTypeError(ctx, "morph.image.frame requires output");
	resolved_output = runner_authorize_path_copy(ctx, output, 1);
	if (!resolved_output) {
		JS_FreeCString(ctx, output);
		return JS_EXCEPTION;
	}
	input_val = js_values_arg_or_prop_retain(ctx, argv[0], "input");
	image = sharp_load_input(ctx, input_val);
	JS_FreeValue(ctx, input_val);
	if (!image) {
		free(resolved_output);
		JS_FreeCString(ctx, output);
		return JS_EXCEPTION;
	}
	memset(&tmp, 0, sizeof(tmp));
	tmp.image = image;
	src = js_canvas_image_surface(&tmp);
	if (!src) {
		g_object_unref(image);
		free(resolved_output);
		JS_FreeCString(ctx, output);
		return JS_ThrowInternalError(ctx, "failed to decode image");
	}
	style = js_values_object_string_prop_copy(ctx, argv[0], "style");
	caption = js_values_object_string_prop_copy(ctx, argv[0], "caption");
	(void)js_values_object_int_prop(ctx, argv[0], "padding", pad, &pad);
	if (pad < 8)
		pad = 8;
	width = cairo_image_surface_get_width(src);
	height = cairo_image_surface_get_height(src);
	frame_w = width + pad * 2;
	frame_h = height + pad * 2;
	if (caption && *caption)
		frame_h += pad;
	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, frame_w, frame_h);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		rc = NB_ERR_IO;
	} else {
		cr = cairo_create(surface);
		if (style && strcmp(style, "neon") == 0)
			cairo_set_source_rgb(cr, 0.02, 0.02, 0.04);
		else if (style && strcmp(style, "kraft") == 0)
			cairo_set_source_rgb(cr, 0.82, 0.70, 0.52);
		else if (style && strcmp(style, "dark") == 0)
			cairo_set_source_rgb(cr, 0.08, 0.08, 0.09);
		else
			cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
		cairo_paint(cr);
		cairo_set_source_surface(cr, src, pad, pad);
		cairo_paint(cr);
		if (style && strcmp(style, "neon") == 0) {
			cairo_set_line_width(cr, 8.0);
			cairo_set_source_rgb(cr, 0.0, 0.95, 1.0);
			cairo_rectangle(cr, pad / 2.0, pad / 2.0, frame_w - pad, frame_h - pad);
			cairo_stroke(cr);
			cairo_set_line_width(cr, 3.0);
			cairo_set_source_rgb(cr, 1.0, 0.1, 0.75);
			cairo_rectangle(cr, pad * 0.7, pad * 0.7,
					frame_w - pad * 1.4, frame_h - pad * 1.4);
			cairo_stroke(cr);
		}
		if (caption && *caption) {
			cairo_select_font_face(cr, "Helvetica", CAIRO_FONT_SLANT_NORMAL,
					       CAIRO_FONT_WEIGHT_NORMAL);
			cairo_set_font_size(cr, (double)pad * 0.45);
			if (style && strcmp(style, "dark") == 0)
				cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
			else
				cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
			cairo_move_to(cr, pad, height + pad * 1.65);
			cairo_show_text(cr, caption);
		}
		rc = js_canvas_write_surface(surface, resolved_output);
	}
	if (cr)
		cairo_destroy(cr);
	if (surface)
		cairo_surface_destroy(surface);
	cairo_surface_destroy(src);
	if (style)
		JS_FreeCString(ctx, style);
	if (caption)
		JS_FreeCString(ctx, caption);
	g_object_unref(image);
	if (rc < 0) {
		free(resolved_output);
		JS_FreeCString(ctx, output);
		return JS_ThrowInternalError(ctx, "failed to write frame: %s", strerror(-rc));
	}
	out = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, out, "output", JS_NewString(ctx, output));
	JS_SetPropertyStr(ctx, out, "width", JS_NewInt32(ctx, frame_w));
	JS_SetPropertyStr(ctx, out, "height", JS_NewInt32(ctx, frame_h));
	free(resolved_output);
	JS_FreeCString(ctx, output);
	return out;
}

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

static const JSCFunctionListEntry image_funcs[] = {
	JS_CFUNC_DEF("open", 1, js_image_open),
	JS_CFUNC_DEF("create", 1, js_image_create),
	JS_CFUNC_DEF("metadata", 1, js_image_metadata_fn),
	JS_CFUNC_DEF("resize", 1, js_image_resize_fn),
	JS_CFUNC_DEF("crop", 1, js_image_crop_fn),
	JS_CFUNC_DEF("extract", 1, js_image_crop_fn),
	JS_CFUNC_DEF("extend", 1, js_image_extend_fn),
	JS_CFUNC_DEF("rotate", 1, js_image_rotate_fn),
	JS_CFUNC_DEF("compose", 1, js_image_compose_fn),
	JS_CFUNC_DEF("convert", 1, js_image_convert_fn),
	JS_CFUNC_DEF("frame", 1, js_image_frame_fn),
};

void js_image_install(JSContext *ctx, JSValueConst morph)
{
	JSRuntime *rt = JS_GetRuntime(ctx);
	struct runner_state *state = js_values_state_borrow(ctx);
	JSClassDef def = {"Sharp", .finalizer = sharp_finalizer};
	JSValue proto;
	JSValue image;

	if (!state->sharp_class_id)
		JS_NewClassID(&state->sharp_class_id);
	JS_NewClass(rt, state->sharp_class_id, &def);
	proto = JS_NewObject(ctx);
	JS_SetPropertyFunctionList(ctx, proto, sharp_proto_funcs,
				   sizeof(sharp_proto_funcs) / sizeof(sharp_proto_funcs[0]));
	JS_SetClassProto(ctx, state->sharp_class_id, proto);
	image = JS_NewObject(ctx);
	JS_SetPropertyFunctionList(ctx, image, image_funcs,
				   sizeof(image_funcs) / sizeof(image_funcs[0]));
	JS_SetPropertyStr(ctx, morph, "image", image);
}
