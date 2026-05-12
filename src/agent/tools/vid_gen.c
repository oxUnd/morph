#include "vid_gen.h"
#include "models/video_gen.h"
#include "render/video.h"
#include "util/log.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static struct model *g_vid_llm;

static int vid_gen_exec(const char *args_json, char **result_json, void *user_data)
{
	(void)user_data;
	if (!result_json)
		return -EINVAL;

	cJSON *root = cJSON_Parse(args_json);
	const char *prompt = NULL;
	const char *image_path = NULL;
	int duration = 5;
	if (root) {
		cJSON *p = cJSON_GetObjectItem(root, "prompt");
		if (cJSON_IsString(p)) prompt = p->valuestring;
		cJSON *img = cJSON_GetObjectItem(root, "image_path");
		if (cJSON_IsString(img)) image_path = img->valuestring;
		cJSON *d = cJSON_GetObjectItem(root, "duration");
		if (cJSON_IsNumber(d)) duration = d->valuedouble;
	}
	if (!prompt) {
		cJSON_Delete(root);
		*result_json = strdup("{\"error\":\"missing prompt\"}");
		return -EINVAL;
	}

	struct video_result vid_res = {0};
	int rc = video_gen_create(g_vid_llm, prompt, image_path, duration, &vid_res);
	cJSON_Delete(root);

	if (rc < 0) {
		*result_json = strdup("{\"error\":\"video generation failed\"}");
		return rc;
	}

	char error[512];
	snprintf(error, sizeof(error),
		 "video generated: %s (%ds)", vid_res.path, vid_res.duration_seconds);
	*result_json = strdup(error);
	log_info("vid_gen: %s", error);

	return 0;
}

int vid_gen_init(struct tool_registry *reg, struct model *video_llm)
{
	if (!reg)
		return -EINVAL;
	g_vid_llm = video_llm;
	return tool_register(reg, "vid_gen",
		"Generate a video from text prompt",
		"{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\"},\"image_path\":{\"type\":\"string\"},\"duration\":{\"type\":\"integer\"}}}",
		vid_gen_exec, NULL);
}
