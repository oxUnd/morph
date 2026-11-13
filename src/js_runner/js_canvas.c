#include "js_canvas.h"
#include "nb/err.h"
#include "nb/cleanup.h"
#include "js_values.h"
#include "js_image.h"
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
#include <pango/pangocairo.h>
#include "stb_image_write.h"

struct surface_pixels {
	cairo_user_data_key_t key;
	unsigned char bytes[];
};
#define CANVAS_DEFAULT_FONT "10px sans-serif"
#define CANVAS_DEFAULT_TEXT_ALIGN "start"
#define CANVAS_DEFAULT_TEXT_BASELINE "alphabetic"
enum canvas_text_align {
	CANVAS_TEXT_ALIGN_LEFT,
	CANVAS_TEXT_ALIGN_CENTER,
	CANVAS_TEXT_ALIGN_RIGHT,
};

enum canvas_text_baseline {
	CANVAS_TEXT_BASELINE_TOP,
	CANVAS_TEXT_BASELINE_HANGING,
	CANVAS_TEXT_BASELINE_MIDDLE,
	CANVAS_TEXT_BASELINE_ALPHABETIC,
	CANVAS_TEXT_BASELINE_IDEOGRAPHIC,
	CANVAS_TEXT_BASELINE_BOTTOM,
};

static void canvas_owner_cleanup(struct canvas **owner);
static void canvas_finalizer(JSRuntime *rt, JSValue val);
static void image_data_finalizer(JSRuntime *rt, JSValue val);
static cairo_status_t png_buf_write(void *closure, const unsigned char *data, unsigned int length);
static JSValue new_canvas_object(JSContext *ctx, int width, int height);
static JSValue js_create_canvas(JSContext *ctx, JSValueConst this_val, int argc,
				JSValueConst *argv);
static JSValue canvas_get_context(JSContext *ctx, JSValueConst this_val, int argc,
				  JSValueConst *argv);
static int parse_color(const char *s, double *r, double *g, double *b, double *a);
static void canvas_set_source(JSContext *ctx, JSValueConst this_val, const char *prop);
static JSValue canvas_rect_op(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
			      int magic);
static JSValue canvas_simple(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
			     int magic);
static JSValue canvas_xy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
			 int magic);
static JSValue canvas_arc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static char *trim_space(char *s);
static void strip_matching_quotes(char *s);
static void canvas_set_font_family(PangoFontDescription *desc, char *family);
static void canvas_parse_font_prefix(PangoFontDescription *desc, char *prefix);
static int canvas_parse_font_string(const char *font, PangoFontDescription *desc);
static PangoFontDescription *canvas_get_font_description(JSContext *ctx, JSValueConst this_val);
static enum canvas_text_align canvas_get_text_align(JSContext *ctx, JSValueConst this_val);
static enum canvas_text_baseline canvas_get_text_baseline(JSContext *ctx, JSValueConst this_val);
static void canvas_move_to_text(JSContext *ctx, JSValueConst this_val, cairo_t *cr,
				PangoLayout *layout, double x, double y);
static JSValue canvas_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
			   int magic);
static JSValue canvas_draw_image(JSContext *ctx, JSValueConst this_val, int argc,
				 JSValueConst *argv);
static JSValue canvas_to_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue canvas_to_buffer(JSContext *ctx, JSValueConst this_val, int argc,
				JSValueConst *argv);
static JSValue js_load_image(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_canvas_create_fn(JSContext *ctx, JSValueConst this_val, int argc,
				   JSValueConst *argv);
static JSValue js_canvas_load_image_fn(JSContext *ctx, JSValueConst this_val, int argc,
				       JSValueConst *argv);
static JSValue js_canvas_to_file_fn(JSContext *ctx, JSValueConst this_val, int argc,
				    JSValueConst *argv);
static JSValue js_canvas_to_buffer_fn(JSContext *ctx, JSValueConst this_val, int argc,
				      JSValueConst *argv);

static void canvas_finalizer(JSRuntime *rt, JSValue val)
{
	struct canvas *canvas = JS_GetOpaque(val, JS_GetClassID(val));

	(void)rt;
	canvas_owner_cleanup(&canvas);
}

static void image_data_finalizer(JSRuntime *rt, JSValue val)
{
	struct image_data *data = JS_GetOpaque(val, JS_GetClassID(val));

	(void)rt;
	if (!data)
		return;
	free(data->data);
	free(data);
}

static cairo_status_t png_buf_write(void *closure, const unsigned char *data, unsigned int length)
{
	nb_buf *buf = closure;

	return (nb_buf_append(buf, (const char *)data, length) ? 0 : NB_ERR_NOMEM) == 0
		       ? CAIRO_STATUS_SUCCESS
		       : CAIRO_STATUS_WRITE_ERROR;
}

int js_canvas_write_surface(cairo_surface_t *surface, const char *path)
{
	unsigned char *src;
	unsigned char *out;
	int width;
	int height;
	int stride;
	int channels;
	int ok;

	if (!surface || !path)
		return NB_ERR_INVAL;
	if (cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32)
		return NB_ERR_INVAL;
	cairo_surface_flush(surface);
	src = cairo_image_surface_get_data(surface);
	width = cairo_image_surface_get_width(surface);
	height = cairo_image_surface_get_height(surface);
	stride = cairo_image_surface_get_stride(surface);
	if (!src || width <= 0 || height <= 0 || stride <= 0)
		return NB_ERR_INVAL;
	channels =
		(js_image_path_ext_is(path, ".jpg") || js_image_path_ext_is(path, ".jpeg")) ? 3 : 4;
	out = malloc((size_t)width * (size_t)height * (size_t)channels);
	if (!out)
		return NB_ERR_NOMEM;
	for (int y = 0; y < height; y++) {
		unsigned char *row = src + (size_t)y * (size_t)stride;
		unsigned char *dst = out + (size_t)y * (size_t)width * (size_t)channels;

		for (int x = 0; x < width; x++) {
			unsigned char b = row[x * 4 + 0];
			unsigned char g = row[x * 4 + 1];
			unsigned char r = row[x * 4 + 2];
			unsigned char a = row[x * 4 + 3];

			if (a > 0 && a < 255) {
				unsigned int ur = ((unsigned int)r * 255U + (unsigned int)a / 2U) /
						  (unsigned int)a;
				unsigned int ug = ((unsigned int)g * 255U + (unsigned int)a / 2U) /
						  (unsigned int)a;
				unsigned int ub = ((unsigned int)b * 255U + (unsigned int)a / 2U) /
						  (unsigned int)a;
				r = (unsigned char)(ur > 255U ? 255U : ur);
				g = (unsigned char)(ug > 255U ? 255U : ug);
				b = (unsigned char)(ub > 255U ? 255U : ub);
			}
			dst[x * channels + 0] = r;
			dst[x * channels + 1] = g;
			dst[x * channels + 2] = b;
			if (channels == 4)
				dst[x * channels + 3] = a;
		}
	}
	if (channels == 3)
		ok = stbi_write_jpg(path, width, height, 3, out, 90);
	else
		ok = stbi_write_png(path, width, height, 4, out, width * 4);
	free(out);
	return ok ? 0 : NB_ERR_IO;
}

static void canvas_owner_cleanup(struct canvas **owner)
{
	struct canvas *canvas = *owner;

	if (!canvas)
		return;
	if (canvas->cr)
		cairo_destroy(canvas->cr);
	if (canvas->surface)
		cairo_surface_destroy(canvas->surface);
	free(canvas->data);
	free(canvas);
	*owner = NULL;
}

static JSValue new_canvas_object(JSContext *ctx, int width, int height)
{
	struct canvas *canvas NB_CLEANUP(canvas_owner_cleanup) = NULL;
	JSValue obj;

	if (width <= 0 || height <= 0 || width > 32768 || height > 32768)
		return JS_ThrowRangeError(ctx, "invalid canvas size");
	canvas = calloc(1, sizeof(*canvas));
	if (!canvas)
		return JS_ThrowOutOfMemory(ctx);
	canvas->width = width;
	canvas->height = height;
	canvas->stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
	if (canvas->stride <= 0)
		return JS_ThrowOutOfMemory(ctx);
	canvas->data = calloc((size_t)canvas->stride, (size_t)height);
	if (!canvas->data)
		return JS_ThrowOutOfMemory(ctx);
	canvas->surface = cairo_image_surface_create_for_data(canvas->data, CAIRO_FORMAT_ARGB32,
							      width, height, canvas->stride);
	if (cairo_surface_status(canvas->surface) != CAIRO_STATUS_SUCCESS)
		return JS_ThrowOutOfMemory(ctx);
	canvas->cr = cairo_create(canvas->surface);
	if (cairo_status(canvas->cr) != CAIRO_STATUS_SUCCESS)
		return JS_ThrowOutOfMemory(ctx);
	obj = JS_NewObjectClass(ctx, js_values_state_borrow(ctx)->canvas_class_id);
	if (JS_IsException(obj))
		return obj;
	JS_SetOpaque(obj, canvas);
	canvas = NULL;
	JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, width));
	JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, height));
	JS_SetPropertyStr(ctx, obj, "font", JS_NewString(ctx, CANVAS_DEFAULT_FONT));
	JS_SetPropertyStr(ctx, obj, "textAlign", JS_NewString(ctx, CANVAS_DEFAULT_TEXT_ALIGN));
	JS_SetPropertyStr(ctx, obj, "textBaseline",
			  JS_NewString(ctx, CANVAS_DEFAULT_TEXT_BASELINE));
	return obj;
}

static JSValue js_create_canvas(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	int32_t width;
	int32_t height;

	(void)this_val;
	if (!js_image_require(ctx))
		return JS_EXCEPTION;
	if (argc < 2)
		return JS_ThrowTypeError(ctx, "morph.canvas.create(width, height)");
	if (JS_ToInt32(ctx, &width, argv[0]) < 0 || JS_ToInt32(ctx, &height, argv[1]) < 0)
		return JS_EXCEPTION;
	return new_canvas_object(ctx, width, height);
}

static JSValue canvas_get_context(JSContext *ctx, JSValueConst this_val, int argc,
				  JSValueConst *argv)
{
	struct canvas *canvas =
		JS_GetOpaque2(ctx, this_val, js_values_state_borrow(ctx)->canvas_class_id);
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

static int parse_color(const char *s, double *r, double *g, double *b, double *a)
{
	unsigned int rv;
	unsigned int gv;
	unsigned int bv;

	if (!s || !r || !g || !b || !a)
		return NB_ERR_INVAL;
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
	if (s[0] == '#' && strlen(s) == 7 && sscanf(s + 1, "%02x%02x%02x", &rv, &gv, &bv) == 3) {
		*r = (double)rv / 255.0;
		*g = (double)gv / 255.0;
		*b = (double)bv / 255.0;
		return 0;
	}
	return NB_ERR_INVAL;
}

static void canvas_set_source(JSContext *ctx, JSValueConst this_val, const char *prop)
{
	struct canvas *canvas =
		JS_GetOpaque(this_val, js_values_state_borrow(ctx)->canvas_class_id);
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

static JSValue canvas_rect_op(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
			      int magic)
{
	struct canvas *canvas =
		JS_GetOpaque2(ctx, this_val, js_values_state_borrow(ctx)->canvas_class_id);
	double x;
	double y;
	double w;
	double h;

	if (!canvas)
		return JS_EXCEPTION;
	if (argc < 4)
		return JS_ThrowTypeError(ctx, "rectangle requires 4 args");
	if (JS_ToFloat64(ctx, &x, argv[0]) < 0 || JS_ToFloat64(ctx, &y, argv[1]) < 0 ||
	    JS_ToFloat64(ctx, &w, argv[2]) < 0 || JS_ToFloat64(ctx, &h, argv[3]) < 0)
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

static JSValue canvas_simple(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
			     int magic)
{
	struct canvas *canvas =
		JS_GetOpaque2(ctx, this_val, js_values_state_borrow(ctx)->canvas_class_id);

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

static JSValue canvas_xy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
			 int magic)
{
	struct canvas *canvas =
		JS_GetOpaque2(ctx, this_val, js_values_state_borrow(ctx)->canvas_class_id);
	double x;
	double y;

	if (!canvas)
		return JS_EXCEPTION;
	if (argc < 2)
		return JS_ThrowTypeError(ctx, "point requires 2 args");
	if (JS_ToFloat64(ctx, &x, argv[0]) < 0 || JS_ToFloat64(ctx, &y, argv[1]) < 0)
		return JS_EXCEPTION;
	if (magic == 1)
		cairo_move_to(canvas->cr, x, y);
	else if (magic == 2)
		cairo_line_to(canvas->cr, x, y);
	else
		cairo_translate(canvas->cr, x, y);
	return JS_UNDEFINED;
}

static JSValue canvas_arc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	struct canvas *canvas =
		JS_GetOpaque2(ctx, this_val, js_values_state_borrow(ctx)->canvas_class_id);
	double x;
	double y;
	double radius;
	double start;
	double end;

	if (!canvas)
		return JS_EXCEPTION;
	if (argc < 5)
		return JS_ThrowTypeError(ctx, "arc(x, y, r, start, end)");
	if (JS_ToFloat64(ctx, &x, argv[0]) < 0 || JS_ToFloat64(ctx, &y, argv[1]) < 0 ||
	    JS_ToFloat64(ctx, &radius, argv[2]) < 0 || JS_ToFloat64(ctx, &start, argv[3]) < 0 ||
	    JS_ToFloat64(ctx, &end, argv[4]) < 0)
		return JS_EXCEPTION;
	cairo_arc(canvas->cr, x, y, radius, start, end);
	return JS_UNDEFINED;
}

static char *trim_space(char *s)
{
	char *end;

	if (!s)
		return NULL;
	while (*s && isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
	return s;
}

static void strip_matching_quotes(char *s)
{
	size_t len;

	if (!s)
		return;
	len = strlen(s);
	if (len < 2)
		return;
	if ((s[0] == '\'' && s[len - 1] == '\'') || (s[0] == '"' && s[len - 1] == '"')) {
		memmove(s, s + 1, len - 2);
		s[len - 2] = '\0';
	}
}

static void canvas_set_font_family(PangoFontDescription *desc, char *family)
{
	char *comma;

	family = trim_space(family);
	if (!family || !*family) {
		pango_font_description_set_family(desc, "Sans");
		return;
	}
	comma = strchr(family, ',');
	if (comma)
		*comma = '\0';
	family = trim_space(family);
	strip_matching_quotes(family);
	if (strcmp(family, "sans-serif") == 0)
		pango_font_description_set_family(desc, "Sans");
	else if (strcmp(family, "serif") == 0)
		pango_font_description_set_family(desc, "Serif");
	else if (strcmp(family, "monospace") == 0)
		pango_font_description_set_family(desc, "Monospace");
	else if (*family)
		pango_font_description_set_family(desc, family);
	else
		pango_font_description_set_family(desc, "Sans");
}

static void canvas_parse_font_prefix(PangoFontDescription *desc, char *prefix)
{
	char *token;
	char *saveptr = NULL;

	for (token = strtok_r(prefix, " \t\r\n", &saveptr); token;
	     token = strtok_r(NULL, " \t\r\n", &saveptr)) {
		if (strcmp(token, "italic") == 0)
			pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
		else if (strcmp(token, "oblique") == 0)
			pango_font_description_set_style(desc, PANGO_STYLE_OBLIQUE);
		else if (strcmp(token, "bold") == 0)
			pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
		else if (strcmp(token, "normal") == 0)
			continue;
		else if (isdigit((unsigned char)token[0])) {
			int weight = atoi(token);

			if (weight >= 100 && weight <= 900)
				pango_font_description_set_weight(desc, (PangoWeight)weight);
		}
	}
}

static int canvas_parse_font_string(const char *font, PangoFontDescription *desc)
{
	char *copy;
	char *px;
	char *size_start;
	char *family;
	double size;

	if (!font || !desc)
		return NB_ERR_INVAL;
	copy = strdup(font);
	if (!copy)
		return NB_ERR_NOMEM;
	px = strstr(copy, "px");
	if (!px) {
		free(copy);
		return NB_ERR_INVAL;
	}
	size_start = px;
	while (size_start > copy) {
		char c = size_start[-1];

		if (!isdigit((unsigned char)c) && c != '.')
			break;
		size_start--;
	}
	if (size_start == px) {
		free(copy);
		return NB_ERR_INVAL;
	}
	size = strtod(size_start, NULL);
	if (size <= 0.0 || size > 4096.0) {
		free(copy);
		return NB_ERR_INVAL;
	}
	*size_start = '\0';
	family = px + 2;
	if (*family == '/') {
		while (*family && !isspace((unsigned char)*family))
			family++;
	}
	pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
	canvas_parse_font_prefix(desc, copy);
	canvas_set_font_family(desc, family);
	free(copy);
	return 0;
}

static PangoFontDescription *canvas_get_font_description(JSContext *ctx, JSValueConst this_val)
{
	PangoFontDescription *desc;
	JSValue val;
	const char *font;

	desc = pango_font_description_new();
	if (!desc)
		return NULL;
	val = JS_GetPropertyStr(ctx, this_val, "font");
	font = JS_ToCString(ctx, val);
	if (!font || canvas_parse_font_string(font, desc) != 0) {
		pango_font_description_set_family(desc, "Sans");
		pango_font_description_set_absolute_size(desc, 10.0 * PANGO_SCALE);
	}
	if (font)
		JS_FreeCString(ctx, font);
	JS_FreeValue(ctx, val);
	return desc;
}

static enum canvas_text_align canvas_get_text_align(JSContext *ctx, JSValueConst this_val)
{
	JSValue val;
	const char *align;
	enum canvas_text_align out = CANVAS_TEXT_ALIGN_LEFT;

	val = JS_GetPropertyStr(ctx, this_val, "textAlign");
	align = JS_ToCString(ctx, val);
	if (align) {
		if (strcmp(align, "center") == 0)
			out = CANVAS_TEXT_ALIGN_CENTER;
		else if (strcmp(align, "right") == 0 || strcmp(align, "end") == 0)
			out = CANVAS_TEXT_ALIGN_RIGHT;
	}
	if (align)
		JS_FreeCString(ctx, align);
	JS_FreeValue(ctx, val);
	return out;
}

static enum canvas_text_baseline canvas_get_text_baseline(JSContext *ctx, JSValueConst this_val)
{
	JSValue val;
	const char *baseline;
	enum canvas_text_baseline out = CANVAS_TEXT_BASELINE_ALPHABETIC;

	val = JS_GetPropertyStr(ctx, this_val, "textBaseline");
	baseline = JS_ToCString(ctx, val);
	if (baseline) {
		if (strcmp(baseline, "top") == 0)
			out = CANVAS_TEXT_BASELINE_TOP;
		else if (strcmp(baseline, "hanging") == 0)
			out = CANVAS_TEXT_BASELINE_HANGING;
		else if (strcmp(baseline, "middle") == 0)
			out = CANVAS_TEXT_BASELINE_MIDDLE;
		else if (strcmp(baseline, "ideographic") == 0)
			out = CANVAS_TEXT_BASELINE_IDEOGRAPHIC;
		else if (strcmp(baseline, "bottom") == 0)
			out = CANVAS_TEXT_BASELINE_BOTTOM;
	}
	if (baseline)
		JS_FreeCString(ctx, baseline);
	JS_FreeValue(ctx, val);
	return out;
}

static void canvas_move_to_text(JSContext *ctx, JSValueConst this_val, cairo_t *cr,
				PangoLayout *layout, double x, double y)
{
	PangoRectangle logical;
	enum canvas_text_align align;
	enum canvas_text_baseline text_baseline;
	double left;
	double top;
	double width;
	double height;
	double baseline;
	double bottom;
	double draw_x;
	double draw_y;
	double anchor_y;

	pango_layout_get_extents(layout, NULL, &logical);
	left = (double)logical.x / (double)PANGO_SCALE;
	top = (double)logical.y / (double)PANGO_SCALE;
	width = (double)logical.width / (double)PANGO_SCALE;
	height = (double)logical.height / (double)PANGO_SCALE;
	baseline = (double)pango_layout_get_baseline(layout) / (double)PANGO_SCALE;
	bottom = top + height;

	align = canvas_get_text_align(ctx, this_val);
	switch (align) {
	case CANVAS_TEXT_ALIGN_CENTER:
		draw_x = x - left - width / 2.0;
		break;
	case CANVAS_TEXT_ALIGN_RIGHT:
		draw_x = x - left - width;
		break;
	case CANVAS_TEXT_ALIGN_LEFT:
	default:
		draw_x = x - left;
		break;
	}

	text_baseline = canvas_get_text_baseline(ctx, this_val);
	switch (text_baseline) {
	case CANVAS_TEXT_BASELINE_TOP:
		anchor_y = top;
		break;
	case CANVAS_TEXT_BASELINE_HANGING:
		anchor_y = top + (baseline - top) * 0.2;
		break;
	case CANVAS_TEXT_BASELINE_MIDDLE:
		anchor_y = top + height / 2.0;
		break;
	case CANVAS_TEXT_BASELINE_IDEOGRAPHIC:
		anchor_y = baseline + (bottom - baseline) * 0.5;
		break;
	case CANVAS_TEXT_BASELINE_BOTTOM:
		anchor_y = bottom;
		break;
	case CANVAS_TEXT_BASELINE_ALPHABETIC:
	default:
		anchor_y = baseline;
		break;
	}
	draw_y = y - anchor_y;
	cairo_move_to(cr, draw_x, draw_y);
}

static JSValue canvas_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
			   int magic)
{
	struct canvas *canvas =
		JS_GetOpaque2(ctx, this_val, js_values_state_borrow(ctx)->canvas_class_id);
	PangoFontDescription *font_desc;
	PangoLayout *layout;
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
	if (JS_ToFloat64(ctx, &x, argv[1]) < 0 || JS_ToFloat64(ctx, &y, argv[2]) < 0) {
		JS_FreeCString(ctx, text);
		return JS_EXCEPTION;
	}
	font_desc = canvas_get_font_description(ctx, this_val);
	if (!font_desc) {
		JS_FreeCString(ctx, text);
		return JS_ThrowOutOfMemory(ctx);
	}
	layout = pango_cairo_create_layout(canvas->cr);
	if (!layout) {
		pango_font_description_free(font_desc);
		JS_FreeCString(ctx, text);
		return JS_ThrowOutOfMemory(ctx);
	}
	pango_layout_set_font_description(layout, font_desc);
	pango_layout_set_text(layout, text, -1);
	pango_cairo_update_layout(canvas->cr, layout);
	if (magic == 1) {
		canvas_set_source(ctx, this_val, "fillStyle");
		canvas_move_to_text(ctx, this_val, canvas->cr, layout, x, y);
		pango_cairo_show_layout(canvas->cr, layout);
	} else {
		canvas_set_source(ctx, this_val, "strokeStyle");
		canvas_move_to_text(ctx, this_val, canvas->cr, layout, x, y);
		pango_cairo_layout_path(canvas->cr, layout);
		cairo_stroke(canvas->cr);
	}
	g_object_unref(layout);
	pango_font_description_free(font_desc);
	JS_FreeCString(ctx, text);
	return JS_UNDEFINED;
}

cairo_surface_t *js_canvas_image_surface(struct sharp_image *img)
{
	cairo_surface_t *surface;
	VipsImage *image;
	VipsImage *cast = NULL;
	void *pixels = NULL;
	unsigned char *data = NULL;
	struct surface_pixels *storage;
	size_t len = 0;
	size_t needed;
	int width;
	int height;
	int bands;
	int stride;

	if (!img || !img->image)
		return NULL;
	image = img->image;
	if (vips_image_get_format(image) != VIPS_FORMAT_UCHAR) {
		if (vips_cast(image, &cast, VIPS_FORMAT_UCHAR, NULL) != 0)
			return NULL;
		image = cast;
	}
	width = vips_image_get_width(image);
	height = vips_image_get_height(image);
	bands = vips_image_get_bands(image);
	if (width <= 0 || height <= 0 || bands <= 0) {
		if (cast)
			g_object_unref(cast);
		return NULL;
	}
	stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
	if (stride <= 0) {
		if (cast)
			g_object_unref(cast);
		return NULL;
	}
	pixels = vips_image_write_to_memory(image, &len);
	if (!pixels) {
		if (cast)
			g_object_unref(cast);
		return NULL;
	}
	needed = (size_t)width * (size_t)height * (size_t)bands;
	if (len < needed) {
		g_free(pixels);
		if (cast)
			g_object_unref(cast);
		return NULL;
	}
	if ((size_t)height > (SIZE_MAX - sizeof(*storage)) / (size_t)stride) {
		g_free(pixels);
		if (cast)
			g_object_unref(cast);
		return NULL;
	}
	storage = calloc(1, sizeof(*storage) + (size_t)stride * (size_t)height);
	if (!storage) {
		g_free(pixels);
		if (cast)
			g_object_unref(cast);
		return NULL;
	}
	data = storage->bytes;
	for (int y = 0; y < height; y++) {
		const unsigned char *src =
			(const unsigned char *)pixels + (size_t)y * (size_t)width * (size_t)bands;
		unsigned char *dst = data + (size_t)y * (size_t)stride;

		for (int x = 0; x < width; x++) {
			unsigned char r;
			unsigned char g;
			unsigned char b;
			unsigned char a = 255;
			unsigned int pr;
			unsigned int pg;
			unsigned int pb;

			if (bands == 1) {
				r = g = b = src[0];
			} else if (bands == 2) {
				r = g = b = src[0];
				a = src[1];
			} else {
				r = src[0];
				g = src[1];
				b = src[2];
				if (bands >= 4)
					a = src[3];
			}
			pr = ((unsigned int)r * (unsigned int)a + 127U) / 255U;
			pg = ((unsigned int)g * (unsigned int)a + 127U) / 255U;
			pb = ((unsigned int)b * (unsigned int)a + 127U) / 255U;
			dst[x * 4 + 0] = (unsigned char)pb;
			dst[x * 4 + 1] = (unsigned char)pg;
			dst[x * 4 + 2] = (unsigned char)pr;
			dst[x * 4 + 3] = a;
			src += bands;
		}
	}
	g_free(pixels);
	if (cast)
		g_object_unref(cast);
	surface = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, width, height,
						      stride);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		free(storage);
		return NULL;
	}
	if (cairo_surface_set_user_data(surface, &storage->key, storage, free) !=
	    CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		free(storage);
		return NULL;
	}
	return surface;
}

static JSValue canvas_draw_image(JSContext *ctx, JSValueConst this_val, int argc,
				 JSValueConst *argv)
{
	struct canvas *canvas =
		JS_GetOpaque2(ctx, this_val, js_values_state_borrow(ctx)->canvas_class_id);
	struct canvas *src_canvas;
	struct sharp_image *src_sharp;
	cairo_surface_t *surface = NULL;
	int destroy_surface = 0;
	double dx;
	double dy;
	double dw;
	double dh;
	int sw;
	int sh;

	if (!canvas)
		return JS_EXCEPTION;
	if (argc < 3)
		return JS_ThrowTypeError(ctx, "drawImage(image, x, y[, w, h])");
	src_canvas = JS_GetOpaque(argv[0], js_values_state_borrow(ctx)->canvas_class_id);
	src_sharp = JS_GetOpaque(argv[0], js_values_state_borrow(ctx)->sharp_class_id);
	if (src_canvas) {
		surface = src_canvas->surface;
		sw = src_canvas->width;
		sh = src_canvas->height;
	} else if (src_sharp) {
		surface = js_canvas_image_surface(src_sharp);
		if (!surface)
			return JS_ThrowInternalError(ctx, "failed to decode image");
		destroy_surface = 1;
		sw = cairo_image_surface_get_width(surface);
		sh = cairo_image_surface_get_height(surface);
	} else {
		return JS_ThrowTypeError(ctx, "drawImage source must be image");
	}
	if (JS_ToFloat64(ctx, &dx, argv[1]) < 0 || JS_ToFloat64(ctx, &dy, argv[2]) < 0) {
		if (destroy_surface)
			cairo_surface_destroy(surface);
		return JS_EXCEPTION;
	}
	dw = (double)sw;
	dh = (double)sh;
	if (argc >= 5 &&
	    (JS_ToFloat64(ctx, &dw, argv[3]) < 0 || JS_ToFloat64(ctx, &dh, argv[4]) < 0)) {
		if (destroy_surface)
			cairo_surface_destroy(surface);
		return JS_EXCEPTION;
	}
	if (dw <= 0.0 || dh <= 0.0 || sw <= 0 || sh <= 0) {
		if (destroy_surface)
			cairo_surface_destroy(surface);
		return JS_ThrowRangeError(ctx, "invalid drawImage size");
	}
	cairo_save(canvas->cr);
	cairo_translate(canvas->cr, dx, dy);
	cairo_scale(canvas->cr, dw / (double)sw, dh / (double)sh);
	cairo_set_source_surface(canvas->cr, surface, 0.0, 0.0);
	cairo_paint(canvas->cr);
	cairo_restore(canvas->cr);
	if (destroy_surface)
		cairo_surface_destroy(surface);
	return JS_UNDEFINED;
}

static JSValue canvas_to_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	struct canvas *canvas =
		JS_GetOpaque2(ctx, this_val, js_values_state_borrow(ctx)->canvas_class_id);
	const char *path;
	char *resolved;
	int rc;

	if (!canvas)
		return JS_EXCEPTION;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "toFile(path)");
	path = JS_ToCString(ctx, argv[0]);
	if (!path)
		return JS_EXCEPTION;
	resolved = runner_authorize_path_copy(ctx, path, 1);
	JS_FreeCString(ctx, path);
	if (!resolved)
		return JS_EXCEPTION;
	rc = js_canvas_write_surface(canvas->surface, resolved);
	free(resolved);
	if (rc < 0)
		return JS_ThrowInternalError(ctx, "failed to write canvas: %s", strerror(-rc));
	return JS_UNDEFINED;
}

static JSValue canvas_to_buffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	struct canvas *canvas =
		JS_GetOpaque2(ctx, this_val, js_values_state_borrow(ctx)->canvas_class_id);
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
	cairo_status_t status;
	JSValue out;

	(void)argc;
	(void)argv;
	if (!canvas)
		return JS_EXCEPTION;
	if ((nb_buf_init(&buf), nb_buf_reserve(&buf, 8192) ? 0 : NB_ERR_NOMEM) != 0)
		return JS_ThrowOutOfMemory(ctx);
	cairo_surface_flush(canvas->surface);
	status = cairo_surface_write_to_png_stream(canvas->surface, png_buf_write, &buf);
	if (status != CAIRO_STATUS_SUCCESS) {
		nb_buf_cleanup(&buf);
		return JS_ThrowInternalError(ctx, "failed to encode canvas");
	}
	out = JS_NewArrayBufferCopy(ctx, (const uint8_t *)buf.ptr, buf.len);
	nb_buf_cleanup(&buf);
	return out;
}

static JSValue js_load_image(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	(void)this_val;
	if (!js_image_require(ctx))
		return JS_EXCEPTION;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "loadImage(path)");
	return js_image_open_value(ctx, JS_UNDEFINED, 1, argv);
}

static JSValue js_canvas_create_fn(JSContext *ctx, JSValueConst this_val, int argc,
				   JSValueConst *argv)
{
	JSValue args[2];
	JSValue out;

	(void)this_val;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "morph.canvas.create(options)");
	if (JS_IsObject(argv[0])) {
		args[0] = JS_GetPropertyStr(ctx, argv[0], "width");
		args[1] = JS_GetPropertyStr(ctx, argv[0], "height");
	} else if (argc >= 2) {
		args[0] = JS_DupValue(ctx, argv[0]);
		args[1] = JS_DupValue(ctx, argv[1]);
	} else {
		return JS_ThrowTypeError(ctx, "morph.canvas.create(options)");
	}
	out = js_create_canvas(ctx, JS_UNDEFINED, 2, args);
	JS_FreeValue(ctx, args[0]);
	JS_FreeValue(ctx, args[1]);
	return out;
}

static JSValue js_canvas_load_image_fn(JSContext *ctx, JSValueConst this_val, int argc,
				       JSValueConst *argv)
{
	JSValue input;
	JSValue out;

	(void)this_val;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "morph.canvas.loadImage(input)");
	input = js_values_arg_or_prop_retain(ctx, argv[0], "input");
	out = js_load_image(ctx, JS_UNDEFINED, 1, &input);
	JS_FreeValue(ctx, input);
	return out;
}

static JSValue js_canvas_to_file_fn(JSContext *ctx, JSValueConst this_val, int argc,
				    JSValueConst *argv)
{
	JSValue canvas;
	JSValue output;
	JSValue out;

	(void)this_val;
	(void)argc;
	if (argc < 1 || !JS_IsObject(argv[0]))
		return JS_ThrowTypeError(ctx, "morph.canvas.toFile(options)");
	canvas = JS_GetPropertyStr(ctx, argv[0], "canvas");
	output = JS_GetPropertyStr(ctx, argv[0], "output");
	out = canvas_to_file(ctx, canvas, 1, &output);
	JS_FreeValue(ctx, canvas);
	JS_FreeValue(ctx, output);
	return out;
}

static JSValue js_canvas_to_buffer_fn(JSContext *ctx, JSValueConst this_val, int argc,
				      JSValueConst *argv)
{
	JSValue canvas;
	JSValue out;

	(void)this_val;
	(void)argc;
	if (argc < 1 || !JS_IsObject(argv[0]))
		return JS_ThrowTypeError(ctx, "morph.canvas.toBuffer(options)");
	canvas = JS_GetPropertyStr(ctx, argv[0], "canvas");
	out = canvas_to_buffer(ctx, canvas, 0, NULL);
	JS_FreeValue(ctx, canvas);
	return out;
}

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
	JS_CFUNC_DEF("drawImage", 5, canvas_draw_image),
	JS_CFUNC_DEF("toFile", 1, canvas_to_file),
	JS_CFUNC_DEF("toBuffer", 1, canvas_to_buffer),
};

static const JSCFunctionListEntry canvas_funcs[] = {
	JS_CFUNC_DEF("create", 1, js_canvas_create_fn),
	JS_CFUNC_DEF("loadImage", 1, js_canvas_load_image_fn),
	JS_CFUNC_DEF("toFile", 1, js_canvas_to_file_fn),
	JS_CFUNC_DEF("toBuffer", 1, js_canvas_to_buffer_fn),
};

void js_canvas_install(JSContext *ctx, JSValueConst morph)
{
	JSRuntime *rt = JS_GetRuntime(ctx);
	struct runner_state *state = js_values_state_borrow(ctx);
	JSClassDef canvas_def = {"Canvas", .finalizer = canvas_finalizer};
	JSClassDef data_def = {"ImageData", .finalizer = image_data_finalizer};
	JSValue proto;
	JSValue canvas;

	if (!state->canvas_class_id)
		JS_NewClassID(&state->canvas_class_id);
	if (!state->image_data_class_id)
		JS_NewClassID(&state->image_data_class_id);
	JS_NewClass(rt, state->canvas_class_id, &canvas_def);
	JS_NewClass(rt, state->image_data_class_id, &data_def);
	proto = JS_NewObject(ctx);
	JS_SetPropertyFunctionList(ctx, proto, canvas_proto_funcs,
				   sizeof(canvas_proto_funcs) / sizeof(canvas_proto_funcs[0]));
	JS_SetClassProto(ctx, state->canvas_class_id, proto);
	canvas = JS_NewObject(ctx);
	JS_SetPropertyFunctionList(ctx, canvas, canvas_funcs,
				   sizeof(canvas_funcs) / sizeof(canvas_funcs[0]));
	JS_SetPropertyStr(ctx, morph, "canvas", canvas);
}
