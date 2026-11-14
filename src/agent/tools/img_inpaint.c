#include "img_inpaint.h"
#include "nb/str.h"
#include "nb/utf8.h"
#include "cJSON.h"
#include "nb/cleanup.h"
#include "annotation.h"
#include "agent/tool_runtime.h"
#include "models/llm.h"
#include "agent/tool_context.h"
#include "models/image_gen.h"
#include "nb/log.h"
#include "errors.h"
#include "nb/buf.h"
#include "nb/vec.h"
#include "media/image.h"
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
	int image_index;
	int W;
	int H;
	int applied;
	char current_ref[PATH_MAX];
	char final_path[PATH_MAX];
};

/* Find existing per-image state by index, or append a fresh zeroed entry. */
static struct inpaint_img *img_state_for(nb_vec *arr, int idx)
{
	struct inpaint_img *it;
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(arr) && (it = (struct inpaint_img *)nb_vec_at(arr, _i_)); _i_++) {
		if (it->image_index == idx)
			return it;
	}
	it = nb_vec_push(arr);
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
	local.image_llm = model_request_view_init(
		local.image_llm, result && result->runtime ? &result->runtime->http : NULL,
		&request_model);
	struct img_inpaint_context *ctx = user_data ? &local : NULL;
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

	if (annotation.bboxes.len == 0) {
		annotation_cleanup(&annotation);
		cJSON_Delete(root);
		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"img_inpaint needs at least one bbox with a "
				"label. For arrow-based cross-image fusion use img_compose.\"}");
		return NB_ERR_INVAL;
	}

	const char *output_dir = tctx ? tool_context_output_dir_borrow(tctx) : NULL;

	nb_vec imgs;
	struct inpaint_img *it;
	if ((nb_vec_init(&imgs, sizeof(*it)), nb_vec_reserve(&imgs, 16) ? 0 : NB_ERR_NOMEM) != 0) {
		annotation_cleanup(&annotation);
		cJSON_Delete(root);
		(void)tool_result_success_json_text_copy(result, "{\"error\":\"oom\"}");
		return NB_ERR_NOMEM;
	}

	int hard_error = 0;
	char err_msg[256] = {0};

	const struct annotation_bbox *bbox;
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&annotation.bboxes) &&
	     (bbox = (const struct annotation_bbox *)nb_vec_at(&annotation.bboxes, _i_));
	     _i_++) {
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
				annotation_image_borrow(&annotation, idx);
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
				(void)nb_str_copy_to(
					nb_str_from_parts(
						p, nb_utf8_clamp_bytes(p, sizeof(resolved_in) - 1)),
					resolved_in, (sizeof(resolved_in) - 1) + 1);
				resolved_in[sizeof(resolved_in) - 1] = '\0';
			}
			(void)nb_str_copy_to(
				nb_str_from_parts(
					resolved_in,
					nb_utf8_clamp_bytes(resolved_in,
							    sizeof(st_img->current_ref) - 1)),
				st_img->current_ref, (sizeof(st_img->current_ref) - 1) + 1);
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

		nb_buf pb NB_CLEANUP(nb_buf_cleanup) = {0};
		if ((nb_buf_init(&pb), nb_buf_reserve(&pb, 512) ? 0 : NB_ERR_NOMEM) != 0) {
			snprintf(err_msg, sizeof(err_msg), "oom");
			hard_error = 1;
			break;
		}
		nb_buf_printf(&pb,
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
			nb_buf_printf(&pb, " Additional direction: %s", user_prompt);

		struct image_result r = {0};
		const char *references[] = {st_img->current_ref};
		int rc = image_gen_create(ctx->image_llm, nb_buf_cstr(&pb), style, size, references,
					  1, output_dir, &r);
		nb_buf_cleanup(&pb);
		if (rc < 0) {
			snprintf(err_msg, sizeof(err_msg), "image edit failed for label '%s'",
				 label);
			hard_error = 1;
			break;
		}

		(void)nb_str_copy_to(
			nb_str_from_parts(r.path, nb_utf8_clamp_bytes(
							  r.path, sizeof(st_img->current_ref) - 1)),
			st_img->current_ref, (sizeof(st_img->current_ref) - 1) + 1);
		st_img->current_ref[sizeof(st_img->current_ref) - 1] = '\0';
		(void)nb_str_copy_to(
			nb_str_from_parts(r.path, nb_utf8_clamp_bytes(
							  r.path, sizeof(st_img->final_path) - 1)),
			st_img->final_path, (sizeof(st_img->final_path) - 1) + 1);
		st_img->final_path[sizeof(st_img->final_path) - 1] = '\0';
		st_img->applied++;
	}

	cJSON *results = cJSON_CreateArray();
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&imgs) && (it = (struct inpaint_img *)nb_vec_at(&imgs, _i_)); _i_++) {
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
	cJSON *it_json = NULL;
	cJSON_ArrayForEach (it_json, results) {
		cJSON *path = cJSON_GetObjectItem(it_json, "output_path");
		if (cJSON_IsString(path) && path->valuestring)
			(void)tool_result_add_image_copy(result, path->valuestring, 0, 0);
	}
	char *out_str NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(out);
	cJSON_Delete(out);
	nb_vec_cleanup(&imgs);
	annotation_cleanup(&annotation);
	cJSON_Delete(root);

	(void)tool_result_success_json_text_copy(result, out_str ? out_str : "{\"error\":\"oom\"}");
	return hard_error && produced == 0 ? NB_ERR_IO : 0;
}

int img_inpaint_init(struct tool_registry *reg, struct model *image_llm, struct tool_context *tctx)
{
	if (!reg)
		return NB_ERR_INVAL;

	struct img_inpaint_context *ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return NB_ERR_NOMEM;
	ctx->image_llm = image_llm;
	ctx->tctx = tctx;

	struct tool_spec spec NB_CLEANUP(tool_spec_cleanup) = {
		.origin = TOOL_ORIGIN_BUILTIN,
		.flags = TOOL_FLAG_MEDIA,
		.name = "img_inpaint",
		.description = "Regenerate labeled regions of an image. A bbox + label means "
			       "\"generate this content inside this box\". Pass the img_annotate "
			       "output (images[] + bboxes[]) verbatim; for each box the tool "
			       "deterministically builds a precise percentage-coordinate "
			       "instruction and edits the image via the image model, keeping "
			       "the rest unchanged. Use img_compose instead when the annotation "
			       "has arrows (cross-image fusion).",
		.input_schema = "{\"type\":\"object\",\"properties\":{"
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
		.user_data_destroy = img_inpaint_context_destroy};
	int rc = tool_register_take(reg, &spec);
	return rc;
}
