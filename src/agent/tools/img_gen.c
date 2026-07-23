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

static int img_gen_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct img_gen_context *ctx = user_data;
	struct tool_context *tctx = ctx ? ctx->tctx : NULL;

	if (!result)
		return -EINVAL;

	cJSON *root = cJSON_Parse(args_json);
	const char *prompt = NULL;
	const char *style = NULL;
	const char *size = NULL;
	const char *ref_img = NULL;
	if (root) {
		cJSON *p = cJSON_GetObjectItem(root, "prompt");
		if (cJSON_IsString(p)) prompt = p->valuestring;
		cJSON *s = cJSON_GetObjectItem(root, "style");
		if (cJSON_IsString(s)) style = s->valuestring;
		cJSON *sz = cJSON_GetObjectItem(root, "size");
		if (cJSON_IsString(sz)) size = sz->valuestring;
		cJSON *ri = cJSON_GetObjectItem(root, "reference_image");
		if (cJSON_IsString(ri)) ref_img = ri->valuestring;
	}
	if (!prompt) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"missing 'prompt' parameter. "
			"Usage: img_gen({\\\"prompt\\\": \\\"a cat\\\"})\"}"));
		return -EINVAL;
	}

	if ((ctx && ctx->image_llm
	     ? image_gen_validate_size_for_model(ctx->image_llm, size)
	     : image_gen_validate_size(size)) < 0) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"invalid size for the configured image model; "
			"use auto, 2k, 4k, or a supported WIDTHxHEIGHT\"}"));
		return -EINVAL;
	}

	const char *output_dir = tctx ? tool_context_output_dir(tctx) : NULL;
	char resolved_ref[PATH_MAX];
	const char *ref_to_send = ref_img;
	if (ref_img && *ref_img && tctx) {
		int rc = tool_context_authorize_path(tctx, TOOL_PATH_READ,
						     ref_img, resolved_ref,
						     sizeof(resolved_ref));
		if (rc < 0) {
			cJSON_Delete(root);
			if (rc == -ENOENT)
				(void)tool_result_success_json_text(result, strdup(
					"{\"error\":\"reference image not found\"}"));
			else
				(void)tool_result_success_json_text(result, strdup(
					"{\"error\":\"read path outside workspace: permission denied\"}"));
			return rc;
		}
		ref_to_send = resolved_ref;
	}

	struct image_result img_res = {0};
	int rc = image_gen_create(ctx ? ctx->image_llm : NULL,
				  prompt, style, size, ref_to_send,
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
		return rc;
	}

	size_t msg_len = strlen(img_res.path) + 64;
	char *msg = malloc(msg_len);
	if (!msg)
		return -ENOMEM;
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
			"reference_image for editing. The configured image adapter "
			"handles provider-specific generation, editing, output, and "
			"size rules. Provide prompt, optional style, and optional "
			"size as auto, 2k, 4k, or WIDTHxHEIGHT.",
		.input_schema =
			"{\"type\":\"object\",\"properties\":{"
			"\"prompt\":{\"type\":\"string\",\"description\":"
			"\"Text description of the image to generate\"},"
			"\"style\":{\"type\":\"string\",\"description\":"
			"\"Image style (e.g. realistic, anime, oil_painting)\"},"
			"\"size\":{\"type\":\"string\",\"description\":"
			"\"Image size supported by the configured model: auto, "
			"2k, 4k, or WIDTHxHEIGHT\"},"
			"\"reference_image\":{\"type\":\"string\","
			"\"description\":\"File path to a reference image for "
			"editing\"}},\"required\":[\"prompt\"]}",
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
