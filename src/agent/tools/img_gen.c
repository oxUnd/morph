#include "img_gen.h"
#include "cJSON.h"
#include "nb/cleanup.h"
#include "agent/tool_runtime.h"
#include "models/llm.h"
#include "agent/tool_context.h"
#include "models/image_gen.h"
#include "render/image.h"
#include "nb/log.h"
#include "errors.h"
#include "media/image.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

struct img_gen_context {
	struct model *image_llm;
	struct tool_context *tctx;
};

static void img_gen_context_destroy(void *user_data)
{
	free(user_data);
}

static int img_gen_validate_public_size(const struct model *image_llm,
					const char *size)
{
	if (!size || !*size)
		return NB_ERR_INVAL;
	return image_llm
		? image_gen_validate_size_for_model(image_llm, size)
		: image_gen_validate_size(size);
}

static int img_gen_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct img_gen_context local = {0};
	struct model request_model;

	if (user_data)
		local = *(struct img_gen_context *)user_data;
	local.image_llm = model_request_view_init(
		local.image_llm, result && result->runtime ? &result->runtime->http : NULL,
		&request_model);
	struct img_gen_context *ctx = user_data ? &local : NULL;
	struct tool_context *tctx = ctx ? ctx->tctx : NULL;

	if (!result)
		return NB_ERR_INVAL;

	cJSON *root = cJSON_Parse(args_json);
	const char *prompt = NULL;
	const char *style = NULL;
	const char *size = "2k";
	const char *reference_paths[IMAGE_GEN_MAX_REFERENCE_IMAGES] = {0};
	const char *references_to_send[IMAGE_GEN_MAX_REFERENCE_IMAGES] = {0};
	const char *reference_error = NULL;
	int reference_count = 0;
	if (root) {
		cJSON *p = cJSON_GetObjectItem(root, "prompt");
		if (cJSON_IsString(p)) prompt = p->valuestring;
		cJSON *s = cJSON_GetObjectItem(root, "style");
		if (cJSON_IsString(s)) style = s->valuestring;
		cJSON *sz = cJSON_GetObjectItem(root, "size");
		if (cJSON_IsString(sz) && sz->valuestring[0])
			size = sz->valuestring;
		cJSON *ri = cJSON_GetObjectItem(root, "reference_image");
		cJSON *ris = cJSON_GetObjectItem(root, "reference_images");
		if (ri && ris) {
			reference_error = "use either 'reference_image' or "
				"'reference_images', not both";
		} else if (ri) {
			if (!cJSON_IsString(ri) || !ri->valuestring[0]) {
				reference_error = "'reference_image' must be a "
					"non-empty file path";
			} else {
				reference_paths[0] = ri->valuestring;
				reference_count = 1;
			}
		} else if (ris) {
			if (!cJSON_IsArray(ris)) {
				reference_error = "'reference_images' must be an array";
			} else if (cJSON_GetArraySize(ris) >
				   IMAGE_GEN_MAX_REFERENCE_IMAGES) {
				reference_error = "'reference_images' supports at most "
					"10 images";
			} else {
				reference_count = cJSON_GetArraySize(ris);
				for (int i = 0; i < reference_count; i++) {
					cJSON *item = cJSON_GetArrayItem(ris, i);

					if (!cJSON_IsString(item) ||
					    !item->valuestring[0]) {
						reference_error = "every item in "
							"'reference_images' must be a "
							"non-empty file path";
						break;
					}
					reference_paths[i] = item->valuestring;
				}
			}
		}
	}
	if (!prompt) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"missing 'prompt' parameter. "
				"Usage: img_gen({\\\"prompt\\\": \\\"a cat\\\"})\"}");
		return NB_ERR_INVAL;
	}
	if (reference_error) {
		(void)tool_result_success_format(result, "invalid reference images: %s",
						 reference_error);
		cJSON_Delete(root);
		return NB_ERR_INVAL;
	}

	if (img_gen_validate_public_size(ctx ? ctx->image_llm : NULL,
					 size) < 0) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"invalid size for the configured image model; "
				"use a size supported by that model and adapter\"}");
		return NB_ERR_INVAL;
	}

	const char *output_dir = tctx ? tool_context_output_dir_borrow(tctx) : NULL;
	char resolved_references[IMAGE_GEN_MAX_REFERENCE_IMAGES][PATH_MAX];
	for (int i = 0; i < reference_count; i++) {
		references_to_send[i] = reference_paths[i];
		if (tctx) {
			int rc = tool_context_authorize_path(
				tctx, TOOL_PATH_READ, reference_paths[i],
				resolved_references[i],
				sizeof(resolved_references[i]));

			if (rc < 0) {
				cJSON_Delete(root);
				if (rc == NB_ERR_NOT_FOUND)
					(void)tool_result_success_format(
						result, "reference image#%d not found", i + 1);
				else
					(void)tool_result_success_json_text_copy(
						result, "{\"error\":\"read path outside "
							"workspace: permission denied\"}");
				return rc;
			}
			references_to_send[i] = resolved_references[i];
		}
	}

	struct image_result img_res = {0};
	int rc = image_gen_create(ctx ? ctx->image_llm : NULL,
				  prompt, style, size,
				  reference_count > 0 ? references_to_send : NULL,
				  reference_count,
				  output_dir, &img_res);
	cJSON_Delete(root);

	if (rc < 0) {
		if (ctx && ctx->image_llm &&
		    ctx->image_llm->last_error[0])
			(void)tool_result_success_format(result, "image generation failed: %s",
							 ctx->image_llm->last_error);
		else
			(void)tool_result_success_json_text_copy(
				result, "{\"error\":\"image generation failed\"}");
		return rc;
	}

	size_t msg_len = strlen(img_res.path) + 64;
	char *msg NB_CLEANUP(nb_charp_cleanup) = malloc(msg_len);
	if (!msg)
		return NB_ERR_NOMEM;
	snprintf(msg, msg_len, "image generated: %s (%dx%d)",
		 img_res.path, img_res.width, img_res.height);
	(void)tool_result_success_json_text_copy(result, msg);
	(void)tool_result_add_image_copy(result, img_res.path, img_res.width, img_res.height);
	nb_log_write(result && result->runtime ? result->runtime->http.logger : NULL, NB_LOG_DEBUG,
		     "img_gen: %s", msg);

	return 0;
}

int img_gen_init(struct tool_registry *reg, struct model *image_llm,
		 struct tool_context *tctx)
{
	struct tool_spec spec NB_CLEANUP(tool_spec_cleanup) = {
		.origin = TOOL_ORIGIN_BUILTIN,
		.flags = TOOL_FLAG_MEDIA,
		.name = "img_gen",
		.description = "Generate an image from a text prompt, with optional "
			       "reference_images for editing or composition. Pass all "
			       "references in one call, in the intended order; the prompt "
			       "can refer to them as image#1, image#2, and so on. Up to "
			       "10 reference images are supported. The configured adapter "
			       "handles provider-specific generation, editing, output, and "
			       "size rules. Provide prompt, optional style, and optional "
			       "size as 2k, 4k, WIDTHxHEIGHT, or auto when supported by "
			       "the configured model. The adapter validates dimensions. Size "
			       "defaults to 2k.",
		.input_schema = "{\"type\":\"object\",\"properties\":{"
				"\"prompt\":{\"type\":\"string\",\"description\":"
				"\"Text description of the image to generate\"},"
				"\"style\":{\"type\":\"string\",\"description\":"
				"\"Image style (e.g. realistic, anime, oil_painting)\"},"
				"\"size\":{\"type\":\"string\",\"description\":"
				"\"Image size supported by the configured model: 2k, 4k, "
				"WIDTHxHEIGHT, or auto when supported. Dimensions are validated "
				"by the configured adapter. "
				"Defaults to 2k\",\"default\":\"2k\"},"
				"\"reference_image\":{\"type\":\"string\","
				"\"description\":\"Legacy single reference image path. "
				"Do not combine with reference_images\"},"
				"\"reference_images\":{\"type\":\"array\","
				"\"items\":{\"type\":\"string\"},\"maxItems\":10,"
				"\"description\":\"Ordered reference image paths for "
				"editing or composition. Pass all references in one call; "
				"the prompt may identify them as image#1, image#2, etc. "
				"Do not combine with reference_image\"}},"
				"\"required\":[\"prompt\"]}",
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.exec = img_gen_exec,
		.user_data_destroy = img_gen_context_destroy,
	};
	struct img_gen_context *ctx;
	int rc;

	if (!reg)
		return NB_ERR_INVAL;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return NB_ERR_NOMEM;
	ctx->image_llm = image_llm;
	ctx->tctx = tctx;
	spec.user_data = ctx;

	rc = tool_register_take(reg, &spec);
	return rc;
}
