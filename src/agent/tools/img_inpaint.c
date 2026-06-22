#include "img_inpaint.h"
#include "agent/tool_context.h"
#include "models/image_gen.h"
#include "util/log.h"
#include "util/error.h"
#include "util/buf.h"
#include "util/array.h"
#include "cJSON.h"
#include "stb_image.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

struct img_inpaint_context {
	struct model *image_llm;
	struct tool_context *tctx;
};

static void img_inpaint_context_destroy(void *user_data)
{
	free(user_data);
}

/* Per-source-image running state while bboxes are applied serially. */
struct inpaint_img {
	int  image_index;
	int  W;
	int  H;
	int  applied;
	char current_ref[PATH_MAX];
	char final_path[PATH_MAX];
};

/*
 * Resolve the annotation object from tool args. Accepts:
 *   {"annotation": { images, bboxes, ... }}
 *   {"annotation": "<json string>"}
 *   { images, bboxes, ... }   (top-level)
 * If the annotation was parsed from a string, *owned is set to the parsed
 * root which the caller must cJSON_Delete.
 */
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

static int coords_normalized(double x, double y, double w, double h)
{
	return x >= 0 && x <= 1.0 && y >= 0 && y <= 1.0 &&
	       w > 0 && w <= 1.0 && h > 0 && h <= 1.0;
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

static void image_dims(cJSON *images, int idx, const char *path, int *W, int *H)
{
	*W = 0;
	*H = 0;
	if (cJSON_IsArray(images)) {
		cJSON *im = cJSON_GetArrayItem(images, idx);
		if (im) {
			*W = (int)jnum(im, "width");
			*H = (int)jnum(im, "height");
		}
	}
	if ((*W <= 0 || *H <= 0) && path) {
		int n = 0;
		stbi_info(path, W, H, &n);
	}
}

/* Find existing per-image state by index, or append a fresh zeroed entry. */
static struct inpaint_img *img_state_for(morph_array_t *arr, int idx)
{
	struct inpaint_img *it;
	morph_array_foreach(it, arr, struct inpaint_img) {
		if (it->image_index == idx)
			return it;
	}
	it = morph_array_push(arr);
	if (!it)
		return NULL;
	memset(it, 0, sizeof(*it));
	it->image_index = idx;
	return it;
}

static int img_inpaint_exec(const char *args_json, struct tool_result *result,
			    void *user_data)
{
	struct img_inpaint_context *ctx = user_data;
	struct tool_context *tctx = ctx ? ctx->tctx : NULL;

	if (!result)
		return -EINVAL;

	cJSON *root = cJSON_Parse(args_json);
	if (!root) {
		(void)tool_result_take_text(result, strdup("{\"error\":\"invalid JSON\"}"));
		return -EINVAL;
	}

	if (!ctx || !ctx->image_llm || !ctx->image_llm->api_key[0]) {
		cJSON_Delete(root);
		(void)tool_result_take_text(result, strdup("{\"error\":\"no image model configured\"}"));
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	}

	cJSON *owned = NULL;
	cJSON *ann = get_annotation(root, &owned);
	if (!ann) {
		cJSON_Delete(root);
		(void)tool_result_take_text(result, strdup("{\"error\":\"missing or invalid annotation\"}"));
		return -EINVAL;
	}

	cJSON *images = cJSON_GetObjectItem(ann, "images");
	cJSON *bboxes = cJSON_GetObjectItem(ann, "bboxes");

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

	if (image_gen_validate_size(size) < 0) {
		if (owned)
			cJSON_Delete(owned);
		cJSON_Delete(root);
		(void)tool_result_take_text(result, strdup(
			"{\"error\":\"invalid size: use WIDTHxHEIGHT with total "
			"pixels between 2560x1440 and 4096x4096, or 2k, 3k, 4k\"}"));
		return -EINVAL;
	}

	if (!cJSON_IsArray(bboxes) || cJSON_GetArraySize(bboxes) == 0) {
		if (owned)
			cJSON_Delete(owned);
		cJSON_Delete(root);
		(void)tool_result_take_text(result, strdup(
			"{\"error\":\"img_inpaint needs at least one bbox with a "
			"label. For arrow-based cross-image fusion use img_compose.\"}"));
		return -EINVAL;
	}

	const char *output_dir = tctx ? tool_context_output_dir(tctx) : NULL;

	morph_array_t imgs;
	if (morph_array_init(&imgs, MORPH_ARRAY_INIT_CAP,
			     sizeof(struct inpaint_img)) != 0) {
		if (owned)
			cJSON_Delete(owned);
		cJSON_Delete(root);
		(void)tool_result_take_text(result, strdup("{\"error\":\"oom\"}"));
		return -ENOMEM;
	}

	int hard_error = 0;
	char err_msg[256] = {0};

	int nbox = cJSON_GetArraySize(bboxes);
	for (int i = 0; i < nbox; i++) {
		cJSON *b = cJSON_GetArrayItem(bboxes, i);
		int idx = (int)jnum(b, "image_index");
		if (idx < 0)
			continue;

		cJSON *lbl = cJSON_GetObjectItem(b, "label");
		const char *label = cJSON_IsString(lbl) ? lbl->valuestring : NULL;
		if (!label || !label[0])
			continue;

		struct inpaint_img *st_img = img_state_for(&imgs, idx);
		if (!st_img) {
			snprintf(err_msg, sizeof(err_msg), "oom");
			hard_error = 1;
			break;
		}

		if (st_img->current_ref[0] == '\0') {
			const char *p = image_path_at(images, idx);
			if (!p) {
				snprintf(err_msg, sizeof(err_msg),
					 "no image path for image_index %d", idx);
				hard_error = 1;
				break;
			}
			char resolved_in[PATH_MAX];
			if (tctx) {
				int rc = tool_context_authorize_path(
					tctx, TOOL_PATH_READ, p, resolved_in,
					sizeof(resolved_in));
				if (rc < 0) {
					snprintf(err_msg, sizeof(err_msg),
						 "cannot read image %s "
						 "(outside workspace?)", p);
					hard_error = 1;
					break;
				}
			} else {
				strncpy(resolved_in, p, sizeof(resolved_in) - 1);
				resolved_in[sizeof(resolved_in) - 1] = '\0';
			}
			strncpy(st_img->current_ref, resolved_in,
				sizeof(st_img->current_ref) - 1);
			st_img->current_ref[sizeof(st_img->current_ref) - 1] = '\0';
			image_dims(images, idx, resolved_in, &st_img->W,
				   &st_img->H);
		}

		int W = st_img->W, H = st_img->H;
		double bx = jnum(b, "x"), by = jnum(b, "y");
		double bw = jnum(b, "w"), bh = jnum(b, "h");

		double x0, y0, wp, hp;
		if (coords_normalized(bx, by, bw, bh)) {
			x0 = bx * 100.0;
			y0 = by * 100.0;
			wp = bw * 100.0;
			hp = bh * 100.0;
		} else if (W > 0 && H > 0) {
			x0 = bx / W * 100.0;
			y0 = by / H * 100.0;
			wp = bw / W * 100.0;
			hp = bh / H * 100.0;
		} else {
			x0 = bx * 100.0;
			y0 = by * 100.0;
			wp = bw * 100.0;
			hp = bh * 100.0;
		}
		double x1 = x0 + wp, y1 = y0 + hp;
		double cx = x0 + wp / 2.0, cy = y0 + hp / 2.0;

		morph_buf_t pb;
		if (morph_buf_init(&pb, 512) != 0) {
			snprintf(err_msg, sizeof(err_msg), "oom");
			hard_error = 1;
			break;
		}
		morph_buf_printf(&pb,
			"Edit the provided reference image. Within the "
			"rectangular region spanning horizontally from %.1f%% "
			"to %.1f%% and vertically from %.1f%% to %.1f%% of the "
			"image (region center at %.1f%%, %.1f%%; region size "
			"%.1f%% wide by %.1f%% tall), generate: %s. Keep "
			"everything outside this region unchanged, and match "
			"the surrounding lighting, perspective, color, and "
			"style so the result looks natural and seamless.",
			x0, x1, y0, y1, cx, cy, wp, hp, label);
		if (user_prompt && user_prompt[0])
			morph_buf_printf(&pb, " Additional direction: %s",
					 user_prompt);

		struct image_result r = {0};
		int rc = image_gen_create(ctx->image_llm,
					  morph_buf_cstr(&pb), style, size,
					  st_img->current_ref, output_dir, &r);
		morph_buf_cleanup(&pb);
		if (rc < 0) {
			snprintf(err_msg, sizeof(err_msg),
				 "image edit failed for label '%s'", label);
			hard_error = 1;
			break;
		}

		strncpy(st_img->current_ref, r.path,
			sizeof(st_img->current_ref) - 1);
		st_img->current_ref[sizeof(st_img->current_ref) - 1] = '\0';
		strncpy(st_img->final_path, r.path,
			sizeof(st_img->final_path) - 1);
		st_img->final_path[sizeof(st_img->final_path) - 1] = '\0';
		st_img->applied++;
	}

	cJSON *results = cJSON_CreateArray();
	struct inpaint_img *it;
	morph_array_foreach(it, &imgs, struct inpaint_img) {
		if (it->applied == 0)
			continue;
		cJSON *one = cJSON_CreateObject();
		cJSON_AddNumberToObject(one, "image_index", it->image_index);
		cJSON_AddStringToObject(one, "output_path", it->final_path);
		cJSON_AddNumberToObject(one, "edits_applied", it->applied);
		cJSON_AddItemToArray(results, one);
	}

	cJSON *out = cJSON_CreateObject();
	cJSON_AddItemToObject(out, "results", results);
	if (hard_error)
		cJSON_AddStringToObject(out, "error", err_msg);

	int produced = cJSON_GetArraySize(results);
	char *out_str = cJSON_PrintUnformatted(out);
	cJSON_Delete(out);
	morph_array_cleanup(&imgs);
	if (owned)
		cJSON_Delete(owned);
	cJSON_Delete(root);

	(void)tool_result_take_text(result, out_str ? out_str : strdup("{\"error\":\"oom\"}"));
	return hard_error && produced == 0 ? -EIO : 0;
}

int img_inpaint_init(struct tool_registry *reg, struct model *image_llm,
		     struct tool_context *tctx)
{
	if (!reg)
		return -EINVAL;

	struct img_inpaint_context *ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ctx->image_llm = image_llm;
	ctx->tctx = tctx;

	int rc = tool_register(reg, "img_inpaint",
		"Regenerate labeled regions of an image. A bbox + label means "
		"\"generate this content inside this box\". Pass the img_annotate "
		"output (images[] + bboxes[]) verbatim; for each box the tool "
		"deterministically builds a precise percentage-coordinate "
		"instruction and edits the image via the image model, keeping "
		"the rest unchanged. Use img_compose instead when the annotation "
		"has arrows (cross-image fusion).",
		"{\"type\":\"object\",\"properties\":{"
		"\"annotation\":{\"type\":\"object\",\"description\":\"The full "
		"JSON returned by img_annotate, containing images[] (with path, "
		"width, height) and bboxes[] (with image_index, x, y, w, h, "
		"label). Pass it through verbatim.\"},"
		"\"prompt\":{\"type\":\"string\",\"description\":\"Optional extra "
		"creative direction applied to every region\"},"
		"\"style\":{\"type\":\"string\",\"description\":\"Optional style\"},"
		"\"size\":{\"type\":\"string\",\"description\":\"Optional output "
		"size: WIDTHxHEIGHT with total pixels between 2560x1440 and "
		"4096x4096 inclusive, or 2k, 3k, 4k\"}},"
		"\"required\":[\"annotation\"]}",
		img_inpaint_exec, ctx, img_inpaint_context_destroy);
	if (rc != 0)
		free(ctx);
	return rc;
}
