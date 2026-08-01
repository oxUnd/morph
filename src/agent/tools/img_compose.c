#include "img_compose.h"
#include "agent/tool_context.h"
#include "models/image_gen.h"
#include "util/log.h"
#include "util/file.h"
#include "util/error.h"
#include "util/buf.h"
#include "util/image_util.h"
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

static cJSON *get_annotation(cJSON *root, cJSON **owned)
{
	*owned = NULL;
	cJSON *ann = cJSON_GetObjectItem(root, "annotation");
	if (ann) {
		if (cJSON_IsString(ann)) {
			cJSON *parsed = cJSON_Parse(ann->valuestring);
			*owned = parsed;
			return parsed;
		}
		if (cJSON_IsObject(ann))
			return ann;
	}
	return root;
}

static double jnum(cJSON *o, const char *k)
{
	cJSON *v = cJSON_GetObjectItem(o, k);
	return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

static const char *image_path_at(cJSON *images, int idx)
{
	if (!cJSON_IsArray(images))
		return NULL;
	cJSON *im = cJSON_GetArrayItem(images, idx);
	if (!im)
		return NULL;
	cJSON *p = cJSON_GetObjectItem(im, "path");
	return cJSON_IsString(p) ? p->valuestring : NULL;
}

static unsigned char *load_rgba(const char *path, int *w, int *h)
{
	int n = 0;
	return stbi_load(path, w, h, &n, 4);
}

/* Crop an axis-aligned region (clipped to bounds) into a fresh RGBA buffer. */
static unsigned char *crop_rgba(const unsigned char *src, int sw, int sh,
				int rx, int ry, int rw, int rh)
{
	if (rw <= 0 || rh <= 0)
		return NULL;
	unsigned char *out = malloc((size_t)rw * (size_t)rh * 4);
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
			const unsigned char *sp =
				src + ((size_t)sy * sw + sx) * 4;
			dp[0] = sp[0];
			dp[1] = sp[1];
			dp[2] = sp[2];
			dp[3] = sp[3];
		}
	}
	return out;
}

/* Alpha-blend an RGBA patch onto the canvas at (dst_x, dst_y). */
static void paste_rgba(unsigned char *canvas, int cw, int ch,
		       const unsigned char *patch, int pw, int ph,
		       int dst_x, int dst_y)
{
	for (int y = 0; y < ph; y++) {
		int cy = dst_y + y;
		if (cy < 0 || cy >= ch)
			continue;
		for (int x = 0; x < pw; x++) {
			int cx = dst_x + x;
			if (cx < 0 || cx >= cw)
				continue;
			const unsigned char *sp =
				patch + ((size_t)y * pw + x) * 4;
			unsigned int a = sp[3];
			if (a == 0)
				continue;
			unsigned char *dp =
				canvas + ((size_t)cy * cw + cx) * 4;
			for (int c = 0; c < 3; c++)
				dp[c] = (unsigned char)
					((sp[c] * a + dp[c] * (255 - a)) / 255);
			dp[3] = 255;
		}
	}
}

/* Find a bbox on image img_idx that contains point (px, py) in pixels. */
static int find_source_bbox(cJSON *bboxes, int img_idx, double px, double py,
			    int W, int H, int *rx, int *ry, int *rw, int *rh)
{
	if (!cJSON_IsArray(bboxes))
		return 0;
	int n = cJSON_GetArraySize(bboxes);
	for (int i = 0; i < n; i++) {
		cJSON *b = cJSON_GetArrayItem(bboxes, i);
		if ((int)jnum(b, "image_index") != img_idx)
			continue;
		double bx = jnum(b, "x"), by = jnum(b, "y");
		double bw = jnum(b, "w"), bh = jnum(b, "h");
		if (bx <= 1.0 && by <= 1.0 && bw <= 1.0 && bh <= 1.0) {
			bx *= W;
			by *= H;
			bw *= W;
			bh *= H;
		}
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

/* Target image = the one the most arrows point to (tie: smallest index). */
static int compute_main_index(cJSON *arrows)
{
	int n = cJSON_GetArraySize(arrows);
	int main_idx = -1, best = 0;
	for (int i = 0; i < n; i++) {
		cJSON *to = cJSON_GetObjectItem(cJSON_GetArrayItem(arrows, i),
					       "to");
		if (!to)
			continue;
		int ti = (int)jnum(to, "image_index");
		if (ti < 0)
			continue;
		int c = 0;
		for (int j = 0; j < n; j++) {
			cJSON *tj = cJSON_GetObjectItem(
				cJSON_GetArrayItem(arrows, j), "to");
			if (tj && (int)jnum(tj, "image_index") == ti)
				c++;
		}
		if (c > best || (c == best && (main_idx < 0 || ti < main_idx))) {
			best = c;
			main_idx = ti;
		}
	}
	return main_idx;
}

static int img_compose_exec(const char *args_json, struct tool_result *result,
			    void *user_data)
{
	struct img_compose_context *ctx = user_data;
	struct tool_context *tctx = ctx ? ctx->tctx : NULL;

	if (!result)
		return -EINVAL;

	cJSON *root = cJSON_Parse(args_json);
	if (!root) {
		(void)tool_result_success_json_text(result, strdup("{\"error\":\"invalid JSON\"}"));
		return -EINVAL;
	}

	if (!ctx || !ctx->image_llm || !ctx->image_llm->api_key[0]) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup("{\"error\":\"no image model configured\"}"));
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	}

	cJSON *owned = NULL;
	cJSON *ann = get_annotation(root, &owned);
	if (!ann) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup("{\"error\":\"missing or invalid annotation\"}"));
		return -EINVAL;
	}

	cJSON *images = cJSON_GetObjectItem(ann, "images");
	cJSON *bboxes = cJSON_GetObjectItem(ann, "bboxes");
	cJSON *arrows = cJSON_GetObjectItem(ann, "arrows");

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
		if (owned)
			cJSON_Delete(owned);
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"invalid size for the configured image model\"}"));
		return -EINVAL;
	}

	if (!cJSON_IsArray(arrows) || cJSON_GetArraySize(arrows) == 0) {
		if (owned)
			cJSON_Delete(owned);
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"img_compose needs arrows (arrow + label = "
			"cross-image fusion). For bbox region generation use "
			"img_inpaint.\"}"));
		return -EINVAL;
	}

	int main_idx = compute_main_index(arrows);
	if (main_idx < 0) {
		if (owned)
			cJSON_Delete(owned);
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"could not determine target image from arrows\"}"));
		return -EINVAL;
	}

	const char *main_path = image_path_at(images, main_idx);
	if (!main_path) {
		if (owned)
			cJSON_Delete(owned);
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup("{\"error\":\"no path for target image\"}"));
		return -EINVAL;
	}

	char resolved_main[PATH_MAX];
	if (tctx) {
		int rc = tool_context_authorize_path(tctx, TOOL_PATH_READ,
						     main_path, resolved_main,
						     sizeof(resolved_main));
		if (rc < 0) {
			if (owned)
				cJSON_Delete(owned);
			cJSON_Delete(root);
			(void)tool_result_success_json_text(result, strdup(
				"{\"error\":\"cannot read target image "
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
		if (owned)
			cJSON_Delete(owned);
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup("{\"error\":\"failed to load target image\"}"));
		MORPH_RETURN(MORPH_ERR_FORMAT);
	}

	cJSON *labels = cJSON_CreateArray();
	int fused = 0;
	int narr = cJSON_GetArraySize(arrows);
	for (int i = 0; i < narr; i++) {
		cJSON *a = cJSON_GetArrayItem(arrows, i);
		cJSON *from = cJSON_GetObjectItem(a, "from");
		cJSON *to = cJSON_GetObjectItem(a, "to");
		if (!from || !to)
			continue;
		if ((int)jnum(to, "image_index") != main_idx)
			continue;

		int from_idx = (int)jnum(from, "image_index");
		const char *src_path = image_path_at(images, from_idx);
		if (!src_path)
			continue;

		char resolved_src[PATH_MAX];
		if (tctx) {
			int rc = tool_context_authorize_path(
				tctx, TOOL_PATH_READ, src_path, resolved_src,
				sizeof(resolved_src));
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

		double fx = jnum(from, "x"), fy = jnum(from, "y");
		double tx = jnum(to, "x"), ty = jnum(to, "y");
		/* Normalized point coordinates -> pixels. */
		if (fx <= 1.0 && fy <= 1.0) {
			fx *= sw;
			fy *= sh;
		}
		if (tx <= 1.0 && ty <= 1.0) {
			tx *= cw;
			ty *= ch;
		}

		int rx, ry, rw, rh;
		if (!find_source_bbox(bboxes, from_idx, fx, fy, sw, sh,
				      &rx, &ry, &rw, &rh)) {
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

		unsigned char *resized = malloc((size_t)tw * (size_t)th * 4);
		if (!resized) {
			free(crop);
			continue;
		}
		if (!stbir_resize_uint8_linear(crop, rw, rh, 0, resized, tw, th,
					       0, (stbir_pixel_layout)4)) {
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

		cJSON *lbl = cJSON_GetObjectItem(a, "label");
		if (cJSON_IsString(lbl))
			cJSON_AddItemToArray(labels,
				cJSON_CreateString(lbl->valuestring));
	}

	if (fused == 0) {
		free(canvas);
		cJSON_Delete(labels);
		if (owned)
			cJSON_Delete(owned);
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"no arrows could be applied to the target image\"}"));
		return -EINVAL;
	}

	/* Write the rough pre-composite draft. */
	const char *odir_in = tctx ? tool_context_output_dir(tctx) : NULL;
	char *out_dir = file_expand_path(odir_in && odir_in[0]
					 ? odir_in : "~/.morph/output");
	if (!out_dir) {
		free(canvas);
		cJSON_Delete(labels);
		if (owned)
			cJSON_Delete(owned);
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup("{\"error\":\"failed to resolve output dir\"}"));
		return -ENOMEM;
	}
	file_ensure_dir(out_dir);
	char draft_path[PATH_MAX];
	snprintf(draft_path, sizeof(draft_path), "%s/img_compose_draft_%lld.png",
		 out_dir, (long long)time(NULL));
	int wrc = stbi_write_png(draft_path, cw, ch, 4, canvas, cw * 4);
	free(canvas);
	if (!wrc) {
		free(out_dir);
		cJSON_Delete(labels);
		if (owned)
			cJSON_Delete(owned);
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup("{\"error\":\"failed to write draft composite\"}"));
		return -EIO;
	}

	/* Build a deterministic harmonization prompt and refine via i2i. */
	morph_buf_t lb;
	morph_buf_init(&lb, 128);
	int nl = cJSON_GetArraySize(labels);
	for (int i = 0; i < nl; i++) {
		cJSON *l = cJSON_GetArrayItem(labels, i);
		if (!cJSON_IsString(l))
			continue;
		morph_buf_printf(&lb, "%s%s", lb.len ? ", " : "",
				 l->valuestring);
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
	int rc = image_gen_create(ctx->image_llm, morph_buf_cstr(&pb), style,
				  size, references, 1, odir_in, &r);
	morph_buf_cleanup(&lb);
	morph_buf_cleanup(&pb);

	cJSON *out = cJSON_CreateObject();
	if (rc < 0) {
		cJSON_AddStringToObject(out, "error",
			"harmonization failed; rough draft saved");
		cJSON_AddStringToObject(out, "draft_path", draft_path);
		(void)tool_result_add_image(result, draft_path, 0, 0);
	} else {
		cJSON_AddStringToObject(out, "output_path", r.path);
		cJSON_AddStringToObject(out, "draft_path", draft_path);
		(void)tool_result_add_image(result, r.path, r.width, r.height);
		(void)tool_result_add_image(result, draft_path, 0, 0);
		cJSON_AddNumberToObject(out, "fused", fused);
		cJSON_AddItemToObject(out, "elements", labels);
		labels = NULL;
	}

	free(out_dir);
	if (labels)
		cJSON_Delete(labels);
	char *out_str = cJSON_PrintUnformatted(out);
	cJSON_Delete(out);
	if (owned)
		cJSON_Delete(owned);
	cJSON_Delete(root);

	(void)tool_result_success_json_text(result, out_str ? out_str : strdup("{\"error\":\"oom\"}"));
	return rc < 0 ? rc : 0;
}

int img_compose_init(struct tool_registry *reg, struct model *image_llm,
		     struct tool_context *tctx)
{
	if (!reg)
		return -EINVAL;

	struct img_compose_context *ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ctx->image_llm = image_llm;
	ctx->tctx = tctx;

	int rc = tool_register(reg, &(struct tool_spec){ .origin = TOOL_ORIGIN_BUILTIN, .name = "img_compose", .description = "Composite/fuse objects across images following annotation "
		"arrows. An arrow + label means \"blend the object at the arrow "
		"source into the target location the arrow points to\". Pass the "
		"img_annotate output (images[], bboxes[], arrows[]) verbatim. "
		"The tool pre-composites the source pixels onto the target at "
		"each arrow's destination (using the source bbox the arrow "
		"starts from), then harmonizes the result via the image model. "
		"Use img_inpaint instead for bbox-only region generation.", .input_schema = "{\"type\":\"object\",\"properties\":{"
		"\"annotation\":{\"type\":\"object\",\"description\":\"The full "
		"JSON returned by img_annotate, containing images[] (path, "
		"width, height), bboxes[] and arrows[] (from/to with "
		"image_index, x, y, and label). Pass it through verbatim.\"},"
		"\"prompt\":{\"type\":\"string\",\"description\":\"Optional extra "
		"creative direction for the blend\"},"
		"\"style\":{\"type\":\"string\",\"description\":\"Optional style\"},"
		"\"size\":{\"type\":\"string\",\"description\":\"Optional output "
		"size: auto, 2k, 4k, or WIDTHxHEIGHT supported by the "
		"configured image model. If omitted, the adapter selects it.\"}},"
		"\"required\":[\"annotation\"]}", .output_schema = TOOL_OBJECT_OUTPUT_SCHEMA, .exec = img_compose_exec, .user_data = ctx, .user_data_destroy = img_compose_context_destroy });
	if (rc != 0)
		free(ctx);
	return rc;
}
