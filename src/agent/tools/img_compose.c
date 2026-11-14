#include "img_compose.h"
#include "cJSON.h"
#include "nb/cleanup.h"
#include "annotation.h"
#include "agent/tool_runtime.h"
#include "models/llm.h"
#include "agent/tool_context.h"
#include "models/image_gen.h"
#include "models/media_output.h"
#include "nb/log.h"
#include "nb/file.h"
#include "errors.h"
#include "nb/buf.h"
#include "media/image.h"
#include "cJSON.h"
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
	return image_load_bounded_copy(path, w, h, &n, 4);
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

	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&annotation->bboxes) &&
	     (bbox = (const struct annotation_bbox *)nb_vec_at(&annotation->bboxes, _i_));
	     _i_++) {
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
	nb_buf bytes;
	int error;
};

static void draft_png_append(void *opaque, void *data, int size)
{
	struct draft_png *draft = opaque;

	if (draft->error == 0)
		draft->error =
			(nb_buf_append(&draft->bytes, data, (size_t)size) ? 0 : NB_ERR_NOMEM);
}

static int img_compose_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct img_compose_context local = {0};
	struct model request_model;

	if (user_data)
		local = *(struct img_compose_context *)user_data;
	local.image_llm = model_request_view_init(
		local.image_llm, result && result->runtime ? &result->runtime->http : NULL,
		&request_model);
	struct img_compose_context *ctx = user_data ? &local : NULL;
	struct tool_context *tctx = ctx ? ctx->tctx : NULL;

	if (!result)
		return NB_ERR_INVAL;

	cJSON *root = cJSON_Parse(args_json);
	if (!root) {
		(void)tool_result_success_json_text_copy(result, "{\"error\":\"invalid JSON\"}");
		return NB_ERR_INVAL;
	}

	if (!ctx || !ctx->image_llm || !model_api_key_borrow(ctx->image_llm)[0]) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"no image model configured\"}");
		return MORPH_ERR_NOT_CONFIGURED;
	}

	struct annotation annotation NB_CLEANUP(annotation_cleanup) = {0};
	int parse_rc = annotation_parse_copy(root, &annotation);

	if (parse_rc != 0) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"missing or invalid annotation\"}");
		return parse_rc;
	}

	const char *user_prompt = NULL, *style = NULL, *size = NULL;
	cJSON *up = cJSON_GetObjectItem(root, "prompt");
	if (cJSON_IsString(up))
		user_prompt = up->valuestring;
	cJSON *st = cJSON_GetObjectItem(root, "style");
	if (cJSON_IsString(st))
		style = st->valuestring;
	cJSON *sz = cJSON_GetObjectItem(root, "size");
	if (cJSON_IsString(sz))
		size = sz->valuestring;

	if (image_gen_validate_size_for_model(ctx->image_llm, size) < 0) {
		annotation_cleanup(&annotation);
		cJSON_Delete(root);
		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"invalid size for the configured image model\"}");
		return NB_ERR_INVAL;
	}

	if (annotation.arrows.len == 0) {
		annotation_cleanup(&annotation);
		cJSON_Delete(root);
		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"img_compose needs arrows (arrow + label = "
				"cross-image fusion). For bbox region generation use "
				"img_inpaint.\"}");
		return NB_ERR_INVAL;
	}

	int main_idx = annotation_main_image(&annotation);
	if (main_idx < 0) {
		annotation_cleanup(&annotation);
		cJSON_Delete(root);
		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"could not determine target image from arrows\"}");
		return NB_ERR_INVAL;
	}

	const struct annotation_image *main_image = annotation_image_borrow(&annotation, main_idx);
	const char *main_path = main_image ? main_image->path : NULL;
	if (!main_path) {
		annotation_cleanup(&annotation);
		cJSON_Delete(root);
		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"no path for target image\"}");
		return NB_ERR_INVAL;
	}

	char resolved_main[PATH_MAX];
	if (tctx) {
		int rc = tool_context_authorize_path(tctx, TOOL_PATH_READ, main_path, resolved_main,
						     sizeof(resolved_main));
		if (rc < 0) {
			annotation_cleanup(&annotation);
			cJSON_Delete(root);
			(void)tool_result_success_json_text_copy(
				result, "{\"error\":\"cannot read target image "
					"(outside workspace?)\"}");
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
		cJSON_Delete(root);
		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"failed to load target image\"}");
		return NB_ERR_FORMAT;
	}

	cJSON *labels = cJSON_CreateArray();
	int fused = 0;
	const struct annotation_arrow *arrow;
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&annotation.arrows) &&
	     (arrow = (const struct annotation_arrow *)nb_vec_at(&annotation.arrows, _i_));
	     _i_++) {
		int from_idx = arrow->from.image_index;
		const struct annotation_image *image;
		const char *src_path;

		if (arrow->to.image_index != main_idx)
			continue;
		image = annotation_image_borrow(&annotation, from_idx);
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
			cJSON_AddItemToArray(labels, cJSON_CreateString(arrow->label));
	}

	if (fused == 0) {
		free(canvas);
		cJSON_Delete(labels);
		annotation_cleanup(&annotation);
		cJSON_Delete(root);
		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"no arrows could be applied to the target image\"}");
		return NB_ERR_INVAL;
	}

	/* Write the rough pre-composite draft. */
	const char *odir_in = tctx ? tool_context_output_dir_borrow(tctx) : NULL;
	char draft_path[PATH_MAX];
	struct draft_png draft = {0};
	int draft_rc =
		(nb_buf_init(&draft.bytes), nb_buf_reserve(&draft.bytes, 4096) ? 0 : NB_ERR_NOMEM);

	if (draft_rc == 0 &&
	    !stbi_write_png_to_func(draft_png_append, &draft, cw, ch, 4, canvas, cw * 4))
		draft_rc = NB_ERR_IO;
	if (draft_rc == 0)
		draft_rc = draft.error;
	if (draft_rc == 0)
		draft_rc = media_output_write(odir_in, "img_compose_draft", "png", draft.bytes.ptr,
					      draft.bytes.len, draft_path, sizeof(draft_path));
	nb_buf_cleanup(&draft.bytes);
	free(canvas);
	if (draft_rc != 0) {
		cJSON_Delete(labels);
		annotation_cleanup(&annotation);
		cJSON_Delete(root);
		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"failed to write draft composite\"}");
		return draft_rc;
	}

	/* Build a deterministic harmonization prompt and refine via i2i. */
	nb_buf lb NB_CLEANUP(nb_buf_cleanup) = {0};
	(nb_buf_init(&lb), nb_buf_reserve(&lb, 128));
	int nl = cJSON_GetArraySize(labels);
	for (int i = 0; i < nl; i++) {
		cJSON *l = cJSON_GetArrayItem(labels, i);
		if (!cJSON_IsString(l))
			continue;
		nb_buf_printf(&lb, "%s%s", lb.len ? ", " : "", l->valuestring);
	}
	int have_labels = lb.len > 0;

	nb_buf pb NB_CLEANUP(nb_buf_cleanup) = {0};
	(nb_buf_init(&pb), nb_buf_reserve(&pb, 512));
	nb_buf_printf(&pb,
		      "The provided image is a rough composite: one or more elements "
		      "have been pasted onto a background. Seamlessly blend the "
		      "inserted element(s)%s%s into the scene — harmonize lighting, "
		      "shadows, perspective, scale, color grading, and edges so the "
		      "result is photorealistic and coherent. Preserve the position "
		      "and identity of each inserted element; only refine how it fits.",
		      have_labels ? ": " : "", nb_buf_cstr(&lb));
	if (user_prompt && user_prompt[0])
		nb_buf_printf(&pb, " Additional direction: %s", user_prompt);

	struct image_result r = {0};
	const char *references[] = {draft_path};
	int rc = image_gen_create(ctx->image_llm, nb_buf_cstr(&pb), style, size, references, 1,
				  odir_in, &r);
	nb_buf_cleanup(&lb);
	nb_buf_cleanup(&pb);

	cJSON *out = cJSON_CreateObject();
	if (rc < 0) {
		cJSON_AddStringToObject(out, "error", "harmonization failed; rough draft saved");
		cJSON_AddStringToObject(out, "draft_path", draft_path);
		(void)tool_result_add_image_copy(result, draft_path, 0, 0);
	} else {
		cJSON_AddStringToObject(out, "output_path", r.path);
		cJSON_AddStringToObject(out, "draft_path", draft_path);
		(void)tool_result_add_image_copy(result, r.path, r.width, r.height);
		(void)tool_result_add_image_copy(result, draft_path, 0, 0);
		cJSON_AddNumberToObject(out, "fused", fused);
		cJSON_AddItemToObject(out, "elements", labels);
		labels = NULL;
	}

	if (labels)
		cJSON_Delete(labels);
	char *out_str NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(out);
	cJSON_Delete(out);
	annotation_cleanup(&annotation);
	cJSON_Delete(root);

	(void)tool_result_success_json_text_copy(result, out_str ? out_str : "{\"error\":\"oom\"}");
	return rc < 0 ? rc : 0;
}

int img_compose_init(struct tool_registry *reg, struct model *image_llm, struct tool_context *tctx)
{
	if (!reg)
		return NB_ERR_INVAL;

	struct img_compose_context *ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return NB_ERR_NOMEM;
	ctx->image_llm = image_llm;
	ctx->tctx = tctx;

	struct tool_spec spec NB_CLEANUP(tool_spec_cleanup) = {
		.origin = TOOL_ORIGIN_BUILTIN,
		.flags = TOOL_FLAG_MEDIA,
		.name = "img_compose",
		.description = "Composite/fuse objects across images following annotation "
			       "arrows. An arrow + label means \"blend the object at the arrow "
			       "source into the target location the arrow points to\". Pass the "
			       "img_annotate output (images[], bboxes[], arrows[]) verbatim. "
			       "The tool pre-composites the source pixels onto the target at "
			       "each arrow's destination (using the source bbox the arrow "
			       "starts from), then harmonizes the result via the image model. "
			       "Use img_inpaint instead for bbox-only region generation.",
		.input_schema = "{\"type\":\"object\",\"properties\":{"
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
		.user_data_destroy = img_compose_context_destroy};
	int rc = tool_register_take(reg, &spec);
	return rc;
}
