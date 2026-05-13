#include "img_gen.h"
#include "models/image_gen.h"
#include "render/image.h"
#include "util/log.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static struct model *g_img_llm;

static int img_gen_exec(const char *args_json, char **result_json, void *user_data)
{
	(void)user_data;
	if (!result_json)
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
		*result_json = strdup("{\"error\":\"missing prompt\"}");
		return -EINVAL;
	}

	if (size) {
		int valid = 0;
		if (strcmp(size, "2k") == 0 || strcmp(size, "3k") == 0 || strcmp(size, "4k") == 0)
			valid = 1;
		else {
			int w, h;
			if (sscanf(size, "%dx%d", &w, &h) == 2)
				valid = 1;
		}
		if (!valid) {
			cJSON_Delete(root);
			*result_json = strdup("{\"error\":\"invalid size, use WIDTHxHEIGHT (e.g. 2048x2048), 2k, 3k, or 4k\"}");
			return -EINVAL;
		}
	}

	struct image_result img_res = {0};
	int rc = image_gen_create(g_img_llm, prompt, style, size, ref_img, &img_res);
	cJSON_Delete(root);

	if (rc < 0) {
		*result_json = strdup("{\"error\":\"image generation failed\"}");
		return rc;
	}

	char error[256];
	snprintf(error, sizeof(error),
		 "image generated: %s (%dx%d)",
		 img_res.path, img_res.width, img_res.height);
	*result_json = strdup(error);
	log_info("img_gen: %s", error);

	return 0;
}

int img_gen_init(struct tool_registry *reg, struct model *image_llm)
{
	if (!reg)
		return -EINVAL;
	g_img_llm = image_llm;
	return tool_register(reg, "img_gen",
		"Generate an image from a text prompt, with optional reference_image for img2img. Provide prompt, optional style, optional size (must be WIDTHxHEIGHT like '2048x2048' or '1024x1024', or '2k'/'3k'/'4k'), optional reference_image (file path to a reference image).",
		"{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\"},\"style\":{\"type\":\"string\"},\"size\":{\"type\":\"string\",\"description\":\"Image size: WIDTHxHEIGHT (e.g. 2048x2048, 1024x1024), 2k, 3k, or 4k\"},\"reference_image\":{\"type\":\"string\"}}}",
		img_gen_exec, NULL);
}