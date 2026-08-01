#include "img_gen.h"
#include "agent/tool_context.h"
#include "models/image_gen.h"
#include "render/image.h"
#include "util/log.h"
#include "util/error.h"
#include "util/image_util.h"
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
	int rc;

	if (!size || !*size || strcmp(size, "auto") == 0)
		MORPH_RETURN(-EINVAL);
	if (strcmp(size, "2k") != 0 && strcmp(size, "4k") != 0 &&
	    image_gen_validate_size(size) < 0)
		MORPH_RETURN(-EINVAL);
	rc = image_llm
		? image_gen_validate_size_for_model(image_llm, size)
		: image_gen_validate_size(size);
	if (rc < 0)
		MORPH_RETURN(rc);
	return 0;
}

static int img_gen_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct img_gen_context *ctx = user_data;
	struct tool_context *tctx = ctx ? ctx->tctx : NULL;

	if (!result)
		MORPH_RETURN(-EINVAL);

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
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"missing 'prompt' parameter. "
			"Usage: img_gen({\\\"prompt\\\": \\\"a cat\\\"})\"}"));
		MORPH_RETURN(-EINVAL);
	}
	if (reference_error) {
		(void)tool_result_successf(result, "invalid reference images: %s",
					   reference_error);
		cJSON_Delete(root);
		MORPH_RETURN(-EINVAL);
	}

	if (img_gen_validate_public_size(ctx ? ctx->image_llm : NULL,
					 size) < 0) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"invalid size for the configured image model; "
			"use 2k, 4k, or WIDTHxHEIGHT with total pixels between "
			"2560x1440 and 4096x4096, subject to model limits\"}"));
		MORPH_RETURN(-EINVAL);
	}

	const char *output_dir = tctx ? tool_context_output_dir(tctx) : NULL;
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
				if (rc == -ENOENT)
					(void)tool_result_successf(
						result,
						"reference image#%d not found",
						i + 1);
				else
					(void)tool_result_success_json_text(
						result, strdup(
						"{\"error\":\"read path outside "
						"workspace: permission denied\"}"));
				MORPH_RETURN(rc);
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
			(void)tool_result_successf(
				result, "image generation failed: %s",
				ctx->image_llm->last_error);
		else
			(void)tool_result_success_json_text(
				result,
				strdup("{\"error\":\"image generation failed\"}"));
		MORPH_RETURN(rc);
	}

	size_t msg_len = strlen(img_res.path) + 64;
	char *msg = malloc(msg_len);
	if (!msg)
		MORPH_RETURN(-ENOMEM);
	snprintf(msg, msg_len, "image generated: %s (%dx%d)",
		 img_res.path, img_res.width, img_res.height);
	(void)tool_result_success_json_text(result, msg);
	(void)tool_result_add_image(result, img_res.path, img_res.width,
				    img_res.height);
	log_dbg("img_gen: %s", msg);

	return 0;
}

int img_gen_init(struct tool_registry *reg, struct model *image_llm,
		 struct tool_context *tctx)
{
	struct tool_spec spec = {
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "img_gen",
		.description =
			"Generate an image from a text prompt, with optional "
			"reference_images for editing or composition. Pass all "
			"references in one call, in the intended order; the prompt "
			"can refer to them as image#1, image#2, and so on. Up to "
			"10 reference images are supported. The configured adapter "
			"handles provider-specific generation, editing, output, and "
			"size rules. Provide prompt, optional style, and optional "
			"size as 2k, 4k, or WIDTHxHEIGHT. Custom dimensions must "
			"have total pixels in the inclusive range from 2560x1440 "
			"through 4096x4096 and satisfy model-specific limits. Size "
			"defaults to 2k.",
		.input_schema =
			"{\"type\":\"object\",\"properties\":{"
			"\"prompt\":{\"type\":\"string\",\"description\":"
			"\"Text description of the image to generate\"},"
			"\"style\":{\"type\":\"string\",\"description\":"
			"\"Image style (e.g. realistic, anime, oil_painting)\"},"
			"\"size\":{\"type\":\"string\",\"description\":"
			"\"Image size supported by the configured model: 2k, 4k, "
			"or WIDTHxHEIGHT. Custom dimensions must have total pixels "
			"between 2560x1440 (2K lower bound) and 4096x4096 (4K "
			"upper bound), inclusive, and satisfy model-specific limits. "
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
		MORPH_RETURN(-EINVAL);

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		MORPH_RETURN(-ENOMEM);
	ctx->image_llm = image_llm;
	ctx->tctx = tctx;
	spec.user_data = ctx;

	rc = tool_register(reg, &spec);
	if (rc != 0)
		free(ctx);
	return rc;
}
