#include "vid_gen.h"
#include "models/video_gen.h"
#include "render/video.h"
#include "util/log.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_REF_IMAGES 16

static struct model *g_vid_llm;

static int vid_gen_exec(const char *args_json, char **result_json, void *user_data)
{
	(void)user_data;
	if (!result_json)
		return -EINVAL;

	cJSON *root = cJSON_Parse(args_json);
	const char *prompt = NULL;
	const char *image_paths[MAX_REF_IMAGES];
	int num_images = 0;
	int duration = 5;

	memset(image_paths, 0, sizeof(image_paths));

	if (root) {
		cJSON *p = cJSON_GetObjectItem(root, "prompt");
		if (cJSON_IsString(p)) prompt = p->valuestring;

		cJSON *ri = cJSON_GetObjectItem(root, "reference_images");
		if (cJSON_IsString(ri)) {
			image_paths[0] = ri->valuestring;
			num_images = 1;
		} else if (cJSON_IsArray(ri)) {
			int n = cJSON_GetArraySize(ri);
			if (n > MAX_REF_IMAGES) n = MAX_REF_IMAGES;
			for (int i = 0; i < n; i++) {
				cJSON *item = cJSON_GetArrayItem(ri, i);
				if (cJSON_IsString(item)) {
					image_paths[num_images++] = item->valuestring;
				}
			}
		}

		cJSON *d = cJSON_GetObjectItem(root, "duration");
		if (cJSON_IsNumber(d)) duration = (int)d->valuedouble;
	}

	if (!prompt) {
		cJSON_Delete(root);
		*result_json = strdup(
			"{\"error\":\"missing 'prompt' parameter. "
			"Usage: vid_gen({\\\"prompt\\\": \\\"a flying drone\\\", "
			"\\\"reference_images\\\": [\\\"img1.jpg\\\", \\\"img2.png\\\"]})\"}");
		return -EINVAL;
	}

	struct video_result vid_res = {0};
	int rc = video_gen_create(g_vid_llm, prompt,
				  num_images > 0 ? image_paths : NULL,
				  num_images, duration, &vid_res);
	cJSON_Delete(root);

	if (rc < 0) {
		*result_json = strdup("{\"error\":\"video generation failed\"}");
		return rc;
	}

	char error[512];
	snprintf(error, sizeof(error),
		 "video generated: %s (%ds)", vid_res.path, vid_res.duration_seconds);
	*result_json = strdup(error);
	log_dbg("vid_gen: %s", error);

	return 0;
}

int vid_gen_init(struct tool_registry *reg, struct model *video_llm)
{
	if (!reg)
		return -EINVAL;
	g_vid_llm = video_llm;
	return tool_register(reg, "vid_gen",
		"Generate a video from a text prompt with optional reference images. "
		"IMPORTANT: Always pass ALL reference images in a single call via the reference_images array. "
		"Never call vid_gen multiple times for the same video. "
		"Images are labeled image#1, image#2, etc. in order. "
		"Use these labels in the prompt to reference specific images, e.g. "
		"\"Generate a video of image#1 walking in the style of image#2\".",
		"{\"type\":\"object\",\"properties\":{"
		"\"prompt\":{\"type\":\"string\",\"description\":\"Text description of the video to generate. Use image#1, image#2, etc. to reference specific images\"},"
		"\"reference_images\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Array of file paths to reference images for img2vid. Pass ALL images in one array, not one at a time. Images are labeled image#1, image#2, etc. in order\"},"
		"\"duration\":{\"type\":\"integer\",\"description\":\"Video duration in seconds\"}"
		"},\"required\":[\"prompt\"]}",
		vid_gen_exec, NULL, NULL);
}
