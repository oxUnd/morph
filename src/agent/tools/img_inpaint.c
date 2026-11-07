#include "img_inpaint.h"
#include "annotation.h"
#include "agent/tool_runtime.h"
#include "models/llm.h"
#include "agent/tool_context.h"
#include "models/image_gen.h"
#include "util/log.h"
#include "util/error.h"
#include "util/buf.h"
#include "util/array.h"
#include "util/image_util.h"
#include "util/json.h"
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
	int image_index;
	int W;
	int H;
	int applied;
	char current_ref[PATH_MAX];
	char final_path[PATH_MAX];
};

/* Find existing per-image state by index, or append a fresh zeroed entry. */
static struct inpaint_img *img_state_for(morph_array_t *arr, int idx)
{
	struct inpaint_img *it;
	morph_array_foreach (it, arr, struct inpaint_img) {
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

static int img_inpaint_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct img_inpaint_context local = {0};
	struct model request_model;

	if (user_data)
		local = *(struct img_inpaint_context *)user_data;
	local.image_llm = model_bind_http_request(
		local.image_llm, result && result->runtime ? &result->runtime->http : NULL,
		&request_model);
	struct img_inpaint_context *ctx = user_data ? &local : NULL;
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

	if (annotation.bboxes.nelts == 0) {
		annotation_cleanup(&annotation);
		json_free(root);
		(void)tool_result_success_json_text(
			result,
			strdup("{\"error\":\"img_inpaint needs at least one bbox with a "
			       "label. For arrow-based cross-image fusion use img_compose.\"}"));
		return -EINVAL;
	}

	const char *output_dir = tctx ? tool_context_output_dir(tctx) : NULL;

	morph_array_t imgs;
	struct inpaint_img *it;
	if (morph_array_init(&imgs, MORPH_ARRAY_INIT_CAP, sizeof(*it)) != 0) {
		annotation_cleanup(&annotation);
		json_free(root);
		(void)tool_result_success_json_text(result, strdup("{\"error\":\"oom\"}"));
		return -ENOMEM;
	}

	int hard_error = 0;
	char err_msg[256] = {0};

	const struct annotation_bbox *bbox;
	morph_array_foreach (bbox, &annotation.bboxes, const struct annotation_bbox) {
		int idx = bbox->image_index;
		const char *label = bbox->label;

		if (!label[0])
			continue;

		struct inpaint_img *st_img = img_state_for(&imgs, idx);
		if (!st_img) {
			snprintf(err_msg, sizeof(err_msg), "oom");
			hard_error = 1;
			break;
		}

		if (st_img->current_ref[0] == '\0') {
			const struct annotation_image *image =
				annotation_image_at(&annotation, idx);
			const char *p = image ? image->path : NULL;
			if (!p) {
				snprintf(err_msg, sizeof(err_msg),
					 "no image path for image_index %d", idx);
				hard_error = 1;
				break;
			}
			char resolved_in[PATH_MAX];
			if (tctx) {
				int rc = tool_context_authorize_path(
					tctx, TOOL_PATH_READ, p, resolved_in, sizeof(resolved_in));
				if (rc < 0) {
					snprintf(err_msg, sizeof(err_msg),
						 "cannot read image %s "
						 "(outside workspace?)",
						 p);
					hard_error = 1;
					break;
				}
			} else {
				strncpy(resolved_in, p, sizeof(resolved_in) - 1);
				resolved_in[sizeof(resolved_in) - 1] = '\0';
			}
			strncpy(st_img->current_ref, resolved_in, sizeof(st_img->current_ref) - 1);
			st_img->current_ref[sizeof(st_img->current_ref) - 1] = '\0';
			st_img->W = image->width;
			st_img->H = image->height;
			if (st_img->W <= 0 || st_img->H <= 0)
				(void)image_probe_size(resolved_in, &st_img->W, &st_img->H);
		}

		struct annotation_rect rect;
		double x0, y0, wp, hp;

		if (annotation_rect_normalize(&bbox->rect, st_img->W, st_img->H, &rect) != 0) {
			snprintf(err_msg, sizeof(err_msg), "invalid image coordinates");
			hard_error = 1;
			break;
		}
		x0 = rect.x * 100.0;
		y0 = rect.y * 100.0;
		wp = rect.width * 100.0;
		hp = rect.height * 100.0;
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
			morph_buf_printf(&pb, " Additional direction: %s", user_prompt);

		struct image_result r = {0};
		const char *references[] = {st_img->current_ref};
		int rc = image_gen_create(ctx->image_llm, morph_buf_cstr(&pb), style, size,
					  references, 1, output_dir, &r);
		morph_buf_cleanup(&pb);
		if (rc < 0) {
			snprintf(err_msg, sizeof(err_msg), "image edit failed for label '%s'",
				 label);
			hard_error = 1;
			break;
		}

		strncpy(st_img->current_ref, r.path, sizeof(st_img->current_ref) - 1);
		st_img->current_ref[sizeof(st_img->current_ref) - 1] = '\0';
		strncpy(st_img->final_path, r.path, sizeof(st_img->final_path) - 1);
		st_img->final_path[sizeof(st_img->final_path) - 1] = '\0';
		st_img->applied++;
	}

	struct json *results = json_array();
	morph_array_foreach (it, &imgs, struct inpaint_img) {
		if (it->applied == 0)
			continue;
		struct json *one = json_object();
		json_add_number(one, "image_index", it->image_index);
		json_add_string(one, "output_path", it->final_path);
		json_add_number(one, "edits_applied", it->applied);
		json_array_add(results, one);
	}

	struct json *out = json_object();
	json_object_add(out, "results", results);
	if (hard_error)
		json_add_string(out, "error", err_msg);

	int produced = json_count(results);
	struct json *it_json = NULL;
	json_foreach (it_json, results) {
		struct json *path = json_member_ci(it_json, "output_path");
		if (json_is_string_value(path) && json_string_value(path))
			(void)tool_result_add_image(result, json_string_value(path), 0, 0);
	}
	char *out_str = json_stringify(out);
	json_free(out);
	morph_array_cleanup(&imgs);
	annotation_cleanup(&annotation);
	json_free(root);

	(void)tool_result_success_json_text(result,
					    out_str ? out_str : strdup("{\"error\":\"oom\"}"));
	return hard_error && produced == 0 ? -EIO : 0;
}

int img_inpaint_init(struct tool_registry *reg, struct model *image_llm, struct tool_context *tctx)
{
	if (!reg)
		return -EINVAL;

	struct img_inpaint_context *ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ctx->image_llm = image_llm;
	ctx->tctx = tctx;

	int rc = tool_register(
		reg,
		&(struct tool_spec){
			.origin = TOOL_ORIGIN_BUILTIN,
			.flags = TOOL_FLAG_MEDIA,
			.name = "img_inpaint",
			.description =
				"Regenerate labeled regions of an image. A bbox + label means "
				"\"generate this content inside this box\". Pass the img_annotate "
				"output (images[] + bboxes[]) verbatim; for each box the tool "
				"deterministically builds a precise percentage-coordinate "
				"instruction and edits the image via the image model, keeping "
				"the rest unchanged. Use img_compose instead when the annotation "
				"has arrows (cross-image fusion).",
			.input_schema =
				"{\"type\":\"object\",\"properties\":{"
				"\"annotation\":{\"type\":\"object\",\"description\":\"The full "
				"JSON returned by img_annotate, containing images[] (with path, "
				"width, height) and bboxes[] (with image_index, x, y, w, h, "
				"label). Pass it through verbatim.\"},"
				"\"prompt\":{\"type\":\"string\",\"description\":\"Optional extra "
				"creative direction applied to every region\"},"
				"\"style\":{\"type\":\"string\",\"description\":\"Optional "
				"style\"},"
				"\"size\":{\"type\":\"string\",\"description\":\"Optional output "
				"size: auto, 2k, 4k, or WIDTHxHEIGHT supported by the "
				"configured image model. If omitted, the adapter selects it.\"}},"
				"\"required\":[\"annotation\"]}",
			.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
			.exec = img_inpaint_exec,
			.user_data = ctx,
			.user_data_destroy = img_inpaint_context_destroy});
	if (rc != 0)
		free(ctx);
	return rc;
}
