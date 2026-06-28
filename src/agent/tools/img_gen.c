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
		(void)tool_result_take_text(result, strdup(
			"{\"error\":\"missing 'prompt' parameter. "
			"Usage: img_gen({\\\"prompt\\\": \\\"a cat\\\"})\"}"));
		return -EINVAL;
	}

	if (image_gen_validate_size(size) < 0) {
		cJSON_Delete(root);
		(void)tool_result_take_text(result, strdup(
			"{\"error\":\"invalid size: use WIDTHxHEIGHT with total "
			"pixels between 2560x1440 and 4096x4096, or 2k, 3k, 4k\"}"));
		return -EINVAL;
	}

	const char *output_dir = tctx ? tool_context_output_dir(tctx) : NULL;
	char resolved_ref[PATH_MAX];
	const char *ref_to_send = ref_img;
	char auto_size[64];
	const char *size_to_send = size;
	if (ref_img && *ref_img && tctx) {
		int rc = tool_context_authorize_path(tctx, TOOL_PATH_READ,
						     ref_img, resolved_ref,
						     sizeof(resolved_ref));
		if (rc < 0) {
			cJSON_Delete(root);
			if (rc == -ENOENT)
				(void)tool_result_take_text(result, strdup(
					"{\"error\":\"reference image not found\"}"));
			else
				(void)tool_result_take_text(result, strdup(
					"{\"error\":\"read path outside workspace: permission denied\"}"));
			return rc;
		}
		ref_to_send = resolved_ref;
	}
	if ((!size || !*size) && ref_to_send && *ref_to_send) {
		int src_w = 0;
		int src_h = 0;
		int out_w = 0;
		int out_h = 0;
		if (image_probe_size(ref_to_send, &src_w, &src_h) == 0 &&
		    image_gen_normalize_reference_size(src_w, src_h,
						       &out_w, &out_h) == 0 &&
		    image_gen_format_size(auto_size, sizeof(auto_size),
					  out_w, out_h) == 0) {
			size_to_send = auto_size;
		}
	}

	struct image_result img_res = {0};
	int rc = image_gen_create(ctx ? ctx->image_llm : NULL,
				  prompt, style, size_to_send, ref_to_send,
				  output_dir, &img_res);
	cJSON_Delete(root);

	if (rc < 0) {
		(void)tool_result_take_text(result, strdup("{\"error\":\"image generation failed\"}"));
		return rc;
	}

	size_t msg_len = strlen(img_res.path) + 64;
	char *msg = malloc(msg_len);
	if (!msg)
		return -ENOMEM;
	snprintf(msg, msg_len, "image generated: %s (%dx%d)",
		 img_res.path, img_res.width, img_res.height);
	(void)tool_result_take_text(result, msg);
	log_dbg("img_gen: %s", msg);

	return 0;
}

int img_gen_init(struct tool_registry *reg, struct model *image_llm,
		 struct tool_context *tctx)
{
	if (!reg)
		return -EINVAL;

	struct img_gen_context *ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ctx->image_llm = image_llm;
	ctx->tctx = tctx;

	int rc = tool_register(reg, "img_gen",
		"Generate an image from a text prompt, with optional reference_image for img2img. Provide prompt, optional style, optional size. size must be WIDTHxHEIGHT with total pixels between 2560x1440 and 4096x4096 inclusive, or 2k/3k/4k. When reference_image is used and the user did not request a different size, inspect the image and pass its WIDTHxHEIGHT; if it is outside the supported pixel range, preserve aspect ratio and scale it into range.",
		"{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\",\"description\":\"Text description of the image to generate\"},\"style\":{\"type\":\"string\",\"description\":\"Image style (e.g. realistic, anime, oil_painting)\"},\"size\":{\"type\":\"string\",\"description\":\"Image size: WIDTHxHEIGHT with total pixels between 2560x1440 and 4096x4096 inclusive, or 2k, 3k, 4k. With reference_image, default to the reference image aspect ratio scaled into this range unless the user explicitly requested a size.\"},\"reference_image\":{\"type\":\"string\",\"description\":\"File path to a reference image for img2img\"}},\"required\":[\"prompt\"]}",
		img_gen_exec, ctx, img_gen_context_destroy);
	if (rc != 0)
		free(ctx);
	return rc;
}
