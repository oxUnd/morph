#include "img_compose.h"
#include "annotation.h"
#include "agent/tool_runtime.h"
#include "models/llm.h"
#include "agent/tool_context.h"
#include "models/image_gen.h"
#include "models/media_output.h"
#include "util/log.h"
#include "util/file.h"
#include "util/error.h"
#include "util/buf.h"
#include "util/image_util.h"
#include "util/json.h"
#include "stb_image.h"
#include "stb_image_write.h"
#include "stb_image_resize2.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <time.h>

struct img_compose_context {
	struct model *image_llm;
	struct tool_context *tctx;
};

static void img_compose_context_destroy(void *user_data)
{
	free(user_data);
}

static unsigned char *load_rgba(const char *path, int *w, int *h)
{
	int n = 0;
	return image_load_bounded(path, w, h, &n, 4);
}

/* Crop an axis-aligned region (clipped to bounds) into a fresh RGBA buffer. */
static unsigned char *crop_rgba(const unsigned char *src, int sw, int sh, int rx, int ry, int rw,
				int rh)
{
	if (rw <= 0 || rh <= 0)
		return NULL;
	size_t bytes;
	if (image_pixel_bytes(rw, rh, 4, &bytes) != 0)
		return NULL;
	unsigned char *out = malloc(bytes);
	if (!out)
		return NULL;
	for (int y = 0; y < rh; y++) {
		for (int x = 0; x < rw; x++) {
			unsigned char *dp = out + ((size_t)y * rw + x) * 4;
			int sx = rx + x, sy = ry + y;
			if (sx < 0 || sx >= sw || sy < 0 || sy >= sh) {
				dp[0] = dp[1] = dp[2] = dp[3] = 0;
				continue;
			}
			const unsigned char *sp = src + ((size_t)sy * sw + sx) * 4;
			dp[0] = sp[0];
			dp[1] = sp[1];
			dp[2] = sp[2];
			dp[3] = sp[3];
		}
	}
	return out;
}

/* Alpha-blend an RGBA patch onto the canvas at (dst_x, dst_y). */
static void paste_rgba(unsigned char *canvas, int cw, int ch, const unsigned char *patch, int pw,
		       int ph, int dst_x, int dst_y)
{
	for (int y = 0; y < ph; y++) {
		int cy = dst_y + y;
		if (cy < 0 || cy >= ch)
			continue;
		for (int x = 0; x < pw; x++) {
			int cx = dst_x + x;
			if (cx < 0 || cx >= cw)
				continue;
			const unsigned char *sp = patch + ((size_t)y * pw + x) * 4;
			unsigned int a = sp[3];
			if (a == 0)
				continue;
			unsigned char *dp = canvas + ((size_t)cy * cw + cx) * 4;
			for (int c = 0; c < 3; c++)
				dp[c] = (unsigned char)((sp[c] * a + dp[c] * (255 - a)) / 255);
			dp[3] = 255;
		}
	}
}

/* Find a bbox on image img_idx that contains point (px, py) in pixels. */
static int find_source_bbox(const struct annotation *annotation, int img_idx, double px, double py,
			    int width, int height, int *rx, int *ry, int *rw, int *rh)
{
	const struct annotation_bbox *bbox;

	morph_array_foreach (bbox, &annotation->bboxes, const struct annotation_bbox) {
		struct annotation_rect rect;
		double bx, by, bw, bh;

		if (bbox->image_index != img_idx ||
		    annotation_rect_normalize(&bbox->rect, width, height, &rect) != 0)
			continue;
		bx = rect.x * width;
		by = rect.y * height;
		bw = rect.width * width;
		bh = rect.height * height;
		if (px >= bx && px <= bx + bw && py >= by && py <= by + bh) {
			*rx = (int)bx;
			*ry = (int)by;
			*rw = (int)bw;
			*rh = (int)bh;
			return 1;
		}
	}
	return 0;
}

struct draft_png {
	morph_buf_t bytes;
	int error;
};

static void draft_png_append(void *opaque, void *data, int size)
{
	struct draft_png *draft = opaque;

	if (draft->error == 0)
		draft->error = morph_buf_append(&draft->bytes, data, (size_t)size);
}

static int img_compose_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct img_compose_context local = {0};
	struct model request_model;

	if (user_data)
		local = *(struct img_compose_context *)user_data;
	local.image_llm = model_bind_http_request(
		local.image_llm, result && result->runtime ? &result->runtime->http : NULL,
		&request_model);
	struct img_compose_context *ctx = user_data ? &local : NULL;
	struct tool_context *tctx = ctx ? ctx->tctx : NULL;

	if (!result)
		return -EINVAL;

	struct json *root = json_parse_prefix(args_json, NULL, NULL);
	if (!root) {
		(void)tool_result_success_json_text(result, strdup("{\"error\":\"invalid JSON\"}"));
		return -EINVAL;
	}

	if (!ctx || !ctx->image_llm || !ctx->image_llm->api_key[0]) {
		json_free(root);
		(void)tool_result_success_json_text(
			result, strdup("{\"error\":\"no image model configured\"}"));
		return MORPH_ERR_NOT_CONFIGURED;
	}

	struct annotation annotation;
	int parse_rc = annotation_parse(root, &annotation);

	if (parse_rc != 0) {
		json_free(root);
		(void)tool_result_success_json_text(
			result, strdup("{\"error\":\"missing or invalid annotation\"}"));
		return parse_rc;
	}

	const char *user_prompt = NULL, *style = NULL, *size = NULL;
	struct json *up = json_member_ci(root, "prompt");
	if (json_is_string_value(up))
		user_prompt = json_string_value(up);
	struct json *st = json_member_ci(root, "style");
	if (json_is_string_value(st))
		style = json_string_value(st);
	struct json *sz = json_member_ci(root, "size");
	if (json_is_string_value(sz))
		size = json_string_value(sz);

	if (image_gen_validate_size_for_model(ctx->image_llm, size) < 0) {
		annotation_cleanup(&annotation);
		json_free(root);
		(void)tool_result_success_json_text(
			result,
			strdup("{\"error\":\"invalid size for the configured image model\"}"));
		return -EINVAL;
	}

	if (annotation.arrows.nelts == 0) {
		annotation_cleanup(&annotation);
		json_free(root);
		(void)tool_result_success_json_text(
			result, strdup("{\"error\":\"img_compose needs arrows (arrow + label = "
				       "cross-image fusion). For bbox region generation use "
				       "img_inpaint.\"}"));
		return -EINVAL;
	}

	int main_idx = annotation_main_image(&annotation);
	if (main_idx < 0) {
		annotation_cleanup(&annotation);
		json_free(root);
		(void)tool_result_success_json_text(
			result,
			strdup("{\"error\":\"could not determine target image from arrows\"}"));
		return -EINVAL;
	}

	const struct annotation_image *main_image = annotation_image_at(&annotation, main_idx);
	const char *main_path = main_image ? main_image->path : NULL;
	if (!main_path) {
		annotation_cleanup(&annotation);
		json_free(root);
		(void)tool_result_success_json_text(
			result, strdup("{\"error\":\"no path for target image\"}"));
		return -EINVAL;
	}

	char resolved_main[PATH_MAX];
	if (tctx) {
		int rc = tool_context_authorize_path(tctx, TOOL_PATH_READ, main_path, resolved_main,
						     sizeof(resolved_main));
		if (rc < 0) {
			annotation_cleanup(&annotation);
			json_free(root);
			(void)tool_result_success_json_text(
				result, strdup("{\"error\":\"cannot read target image "
					       "(outside workspace?)\"}"));
			return rc;
		}
	} else {
		strncpy(resolved_main, main_path, sizeof(resolved_main) - 1);
		resolved_main[sizeof(resolved_main) - 1] = '\0';
	}

	int cw = 0, ch = 0;
	unsigned char *canvas = load_rgba(resolved_main, &cw, &ch);
	if (!canvas) {
		annotation_cleanup(&annotation);
		json_free(root);
		(void)tool_result_success_json_text(
			result, strdup("{\"error\":\"failed to load target image\"}"));
		return MORPH_ERR_FORMAT;
	}

	struct json *labels = json_array();
	int fused = 0;
	const struct annotation_arrow *arrow;
	morph_array_foreach (arrow, &annotation.arrows, const struct annotation_arrow) {
		int from_idx = arrow->from.image_index;
		const struct annotation_image *image;
		const char *src_path;

		if (arrow->to.image_index != main_idx)
			continue;
		image = annotation_image_at(&annotation, from_idx);
		src_path = image ? image->path : NULL;
		if (!src_path)
			continue;

		char resolved_src[PATH_MAX];
		if (tctx) {
			int rc = tool_context_authorize_path(tctx, TOOL_PATH_READ, src_path,
							     resolved_src, sizeof(resolved_src));
			if (rc < 0)
				continue;
		} else {
			strncpy(resolved_src, src_path, sizeof(resolved_src) - 1);
			resolved_src[sizeof(resolved_src) - 1] = '\0';
		}

		int sw = 0, sh = 0;
		unsigned char *src = load_rgba(resolved_src, &sw, &sh);
		if (!src)
			continue;

		double fx, fy, tx, ty;

		if (annotation_point_pixels(&arrow->from, sw, sh, &fx, &fy) != 0 ||
		    annotation_point_pixels(&arrow->to, cw, ch, &tx, &ty) != 0) {
			stbi_image_free(src);
			continue;
		}

		int rx, ry, rw, rh;
		if (!find_source_bbox(&annotation, from_idx, fx, fy, sw, sh, &rx, &ry, &rw, &rh)) {
			/* Fallback: a patch around the arrow source point. */
			rw = (int)(sw * 0.25);
			rh = (int)(sh * 0.25);
			if (rw < 1)
				rw = 1;
			if (rh < 1)
				rh = 1;
			rx = (int)(fx - rw / 2.0);
			ry = (int)(fy - rh / 2.0);
		}

		unsigned char *crop = crop_rgba(src, sw, sh, rx, ry, rw, rh);
		stbi_image_free(src);
		if (!crop)
			continue;

		/* Target size keeps the source object's relative footprint. */
		int tw = (int)((double)rw / sw * cw);
		int th = (int)((double)rh / sh * ch);
		if (tw < 1)
			tw = 1;
		if (th < 1)
			th = 1;

		size_t bytes;
		if (image_pixel_bytes(tw, th, 4, &bytes) != 0) {
			free(crop);
			continue;
		}
		unsigned char *resized = malloc(bytes);
		if (!resized) {
			free(crop);
			continue;
		}
		if (!stbir_resize_uint8_linear(crop, rw, rh, 0, resized, tw, th, 0,
					       (stbir_pixel_layout)4)) {
			free(crop);
			free(resized);
			continue;
		}
		free(crop);

		int dst_x = (int)(tx - tw / 2.0);
		int dst_y = (int)(ty - th / 2.0);
		paste_rgba(canvas, cw, ch, resized, tw, th, dst_x, dst_y);
		free(resized);
		fused++;

		if (arrow->label[0])
			json_array_add(labels, json_string(arrow->label));
	}

	if (fused == 0) {
		free(canvas);
		json_free(labels);
		annotation_cleanup(&annotation);
		json_free(root);
		(void)tool_result_success_json_text(
			result,
			strdup("{\"error\":\"no arrows could be applied to the target image\"}"));
		return -EINVAL;
	}

	/* Write the rough pre-composite draft. */
	const char *odir_in = tctx ? tool_context_output_dir(tctx) : NULL;
	char draft_path[PATH_MAX];
	struct draft_png draft = {0};
	int draft_rc = morph_buf_init(&draft.bytes, 4096);

	if (draft_rc == 0 &&
	    !stbi_write_png_to_func(draft_png_append, &draft, cw, ch, 4, canvas, cw * 4))
		draft_rc = -EIO;
	if (draft_rc == 0)
		draft_rc = draft.error;
	if (draft_rc == 0)
		draft_rc = media_output_write(odir_in, "img_compose_draft", "png", draft.bytes.data,
					      draft.bytes.len, draft_path, sizeof(draft_path));
	morph_buf_cleanup(&draft.bytes);
	free(canvas);
	if (draft_rc != 0) {
		json_free(labels);
		annotation_cleanup(&annotation);
		json_free(root);
		(void)tool_result_success_json_text(
			result, strdup("{\"error\":\"failed to write draft composite\"}"));
		return draft_rc;
	}

	/* Build a deterministic harmonization prompt and refine via i2i. */
	morph_buf_t lb;
	morph_buf_init(&lb, 128);
	int nl = json_count(labels);
	for (int i = 0; i < nl; i++) {
		struct json *l = json_at(labels, i);
		if (!json_is_string_value(l))
			continue;
		morph_buf_printf(&lb, "%s%s", lb.len ? ", " : "", json_string_value(l));
	}
	int have_labels = lb.len > 0;

	morph_buf_t pb;
	morph_buf_init(&pb, 512);
	morph_buf_printf(&pb,
			 "The provided image is a rough composite: one or more elements "
			 "have been pasted onto a background. Seamlessly blend the "
			 "inserted element(s)%s%s into the scene — harmonize lighting, "
			 "shadows, perspective, scale, color grading, and edges so the "
			 "result is photorealistic and coherent. Preserve the position "
			 "and identity of each inserted element; only refine how it fits.",
			 have_labels ? ": " : "", morph_buf_cstr(&lb));
	if (user_prompt && user_prompt[0])
		morph_buf_printf(&pb, " Additional direction: %s", user_prompt);

	struct image_result r = {0};
	const char *references[] = {draft_path};
	int rc = image_gen_create(ctx->image_llm, morph_buf_cstr(&pb), style, size, references, 1,
				  odir_in, &r);
	morph_buf_cleanup(&lb);
	morph_buf_cleanup(&pb);

	struct json *out = json_object();
	if (rc < 0) {
		json_add_string(out, "error", "harmonization failed; rough draft saved");
		json_add_string(out, "draft_path", draft_path);
		(void)tool_result_add_image(result, draft_path, 0, 0);
	} else {
		json_add_string(out, "output_path", r.path);
		json_add_string(out, "draft_path", draft_path);
		(void)tool_result_add_image(result, r.path, r.width, r.height);
		(void)tool_result_add_image(result, draft_path, 0, 0);
		json_add_number(out, "fused", fused);
		json_object_add(out, "elements", labels);
		labels = NULL;
	}

	if (labels)
		json_free(labels);
	char *out_str = json_stringify(out);
	json_free(out);
	annotation_cleanup(&annotation);
	json_free(root);

	(void)tool_result_success_json_text(result,
					    out_str ? out_str : strdup("{\"error\":\"oom\"}"));
	return rc < 0 ? rc : 0;
}

int img_compose_init(struct tool_registry *reg, struct model *image_llm, struct tool_context *tctx)
{
	if (!reg)
		return -EINVAL;

	struct img_compose_context *ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ctx->image_llm = image_llm;
	ctx->tctx = tctx;

	int rc = tool_register(
		reg,
		&(struct tool_spec){
			.origin = TOOL_ORIGIN_BUILTIN,
			.flags = TOOL_FLAG_MEDIA,
			.name = "img_compose",
			.description =
				"Composite/fuse objects across images following annotation "
				"arrows. An arrow + label means \"blend the object at the arrow "
				"source into the target location the arrow points to\". Pass the "
				"img_annotate output (images[], bboxes[], arrows[]) verbatim. "
				"The tool pre-composites the source pixels onto the target at "
				"each arrow's destination (using the source bbox the arrow "
				"starts from), then harmonizes the result via the image model. "
				"Use img_inpaint instead for bbox-only region generation.",
			.input_schema =
				"{\"type\":\"object\",\"properties\":{"
				"\"annotation\":{\"type\":\"object\",\"description\":\"The full "
				"JSON returned by img_annotate, containing images[] (path, "
				"width, height), bboxes[] and arrows[] (from/to with "
				"image_index, x, y, and label). Pass it through verbatim.\"},"
				"\"prompt\":{\"type\":\"string\",\"description\":\"Optional extra "
				"creative direction for the blend\"},"
				"\"style\":{\"type\":\"string\",\"description\":\"Optional "
				"style\"},"
				"\"size\":{\"type\":\"string\",\"description\":\"Optional output "
				"size: auto, 2k, 4k, or WIDTHxHEIGHT supported by the "
				"configured image model. If omitted, the adapter selects it.\"}},"
				"\"required\":[\"annotation\"]}",
			.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
			.exec = img_compose_exec,
			.user_data = ctx,
			.user_data_destroy = img_compose_context_destroy});
	if (rc != 0)
		free(ctx);
	return rc;
}
