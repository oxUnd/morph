#include "vid_gen.h"
#include "agent/tool_context.h"
#include "models/video_gen.h"
#include "render/video.h"
#include "util/log.h"
#include "util/error.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#define MAX_REF_IMAGES 16
#define MAX_REF_VIDEOS 8

struct vid_gen_context {
	struct model *video_llm;
	struct tool_context *tctx;
};

static void vid_gen_context_destroy(void *user_data)
{
	free(user_data);
}

static int is_http_url(const char *s)
{
	return s && (strncmp(s, "http://", 7) == 0 ||
		     strncmp(s, "https://", 8) == 0);
}

static int vid_gen_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct vid_gen_context *ctx = user_data;
	struct tool_context *tctx = ctx ? ctx->tctx : NULL;

	if (!result)
		return -EINVAL;

	cJSON *root = cJSON_Parse(args_json);
	const char *prompt = NULL;
	const char *image_paths[MAX_REF_IMAGES];
	int num_images = 0;
	const char *video_paths[MAX_REF_VIDEOS];
	int num_videos = 0;
	int duration = 5;

	memset(image_paths, 0, sizeof(image_paths));
	memset(video_paths, 0, sizeof(video_paths));

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

		cJSON *rv = cJSON_GetObjectItem(root, "reference_videos");
		if (cJSON_IsString(rv)) {
			video_paths[0] = rv->valuestring;
			num_videos = 1;
		} else if (cJSON_IsArray(rv)) {
			int n = cJSON_GetArraySize(rv);
			if (n > MAX_REF_VIDEOS) n = MAX_REF_VIDEOS;
			for (int i = 0; i < n; i++) {
				cJSON *item = cJSON_GetArrayItem(rv, i);
				if (cJSON_IsString(item)) {
					video_paths[num_videos++] = item->valuestring;
				}
			}
		}

		cJSON *d = cJSON_GetObjectItem(root, "duration");
		if (cJSON_IsNumber(d)) duration = (int)d->valuedouble;
	}

	if (!prompt) {
		cJSON_Delete(root);
		(void)tool_result_take_text(result, strdup(
			"{\"error\":\"missing 'prompt' parameter. "
			"Usage: vid_gen({\\\"prompt\\\": \\\"a flying drone\\\", "
			"\\\"reference_images\\\": [\\\"img1.jpg\\\", \\\"img2.png\\\"], "
			"\\\"reference_videos\\\": [\\\"clip1.mp4\\\"]})\"}"));
		return -EINVAL;
	}

	char resolved_images[MAX_REF_IMAGES][PATH_MAX];
	char resolved_videos[MAX_REF_VIDEOS][PATH_MAX];
	const char *image_paths_to_send[MAX_REF_IMAGES];
	const char *video_paths_to_send[MAX_REF_VIDEOS];

	for (int i = 0; i < num_images; i++) {
		image_paths_to_send[i] = image_paths[i];
		if (tctx && image_paths[i] && image_paths[i][0]) {
			int rc = tool_context_authorize_path(
				tctx, TOOL_PATH_READ, image_paths[i],
				resolved_images[i], sizeof(resolved_images[i]));
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
			image_paths_to_send[i] = resolved_images[i];
		}
	}

	for (int i = 0; i < num_videos; i++) {
		video_paths_to_send[i] = video_paths[i];
		if (tctx && video_paths[i] && video_paths[i][0] &&
		    !is_http_url(video_paths[i])) {
			int rc = tool_context_authorize_path(
				tctx, TOOL_PATH_READ, video_paths[i],
				resolved_videos[i], sizeof(resolved_videos[i]));
			if (rc < 0) {
				cJSON_Delete(root);
				if (rc == -ENOENT)
					(void)tool_result_take_text(result, strdup(
						"{\"error\":\"reference video not found\"}"));
				else
					(void)tool_result_take_text(result, strdup(
						"{\"error\":\"read path outside workspace: permission denied\"}"));
				return rc;
			}
			video_paths_to_send[i] = resolved_videos[i];
		}
	}

	const char *output_dir = tctx ? tool_context_output_dir(tctx) : NULL;
	struct video_result vid_res = {0};
	int rc = video_gen_create(ctx ? ctx->video_llm : NULL, prompt,
				  num_images > 0 ? image_paths_to_send : NULL,
				  num_images,
				  num_videos > 0 ? video_paths_to_send : NULL,
				  num_videos,
				  duration, output_dir, &vid_res);
	cJSON_Delete(root);

	if (rc < 0) {
		if (vid_res.error_msg[0]) {
			char *buf = malloc(strlen(vid_res.error_msg) + 64);
			if (buf) {
				snprintf(buf, strlen(vid_res.error_msg) + 64,
					 "{\"error\":\"%s\"}", vid_res.error_msg);
				(void)tool_result_take_text(result, buf);
			} else {
				(void)tool_result_take_text(result, strdup("{\"error\":\"video generation failed\"}"));
			}
		} else {
			char err_buf[256];
			snprintf(err_buf, sizeof(err_buf),
				 "{\"error\":\"video generation failed: %s\"}",
				 morph_strerror(rc));
			(void)tool_result_take_text(result, strdup(err_buf));
		}
		return rc;
	}

	size_t msg_len = strlen(vid_res.path) + 64;
	char *msg = malloc(msg_len);
	if (!msg)
		return -ENOMEM;
	snprintf(msg, msg_len, "video generated: %s (%ds)",
		 vid_res.path, vid_res.duration_seconds);
	(void)tool_result_take_text(result, msg);
	(void)tool_result_add_video(result, vid_res.path,
				    vid_res.duration_seconds);
	log_dbg("vid_gen: %s", msg);

	return 0;
}

int vid_gen_init(struct tool_registry *reg, struct model *video_llm,
		 struct tool_context *tctx)
{
	if (!reg)
		return -EINVAL;

	struct vid_gen_context *ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ctx->video_llm = video_llm;
	ctx->tctx = tctx;

	int rc = tool_register(TOOL_ORIGIN_BUILTIN, reg, "vid_gen",
		"Generate a video from a text prompt with optional reference images and/or reference videos. "
		"IMPORTANT: Always pass ALL reference assets in a single call via the reference_images / reference_videos arrays. "
		"Never call vid_gen multiple times for the same video. "
		"Images are labeled image#1, image#2, etc.; videos are labeled video#1, video#2, etc., in order. "
		"Use these labels in the prompt to reference specific assets, e.g. "
		"\"Generate a video of image#1 walking in the style of video#1\". "
		"reference_videos accepts local file paths (mp4/mov/webm/mkv/avi) or http(s) URLs.",
		"{\"type\":\"object\",\"properties\":{"
		"\"prompt\":{\"type\":\"string\",\"description\":\"Text description of the video to generate. Use image#1, image#2, video#1, etc. to reference specific assets\"},"
		"\"reference_images\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Array of file paths to reference images for img2vid. Pass ALL images in one array, not one at a time. Images are labeled image#1, image#2, etc. in order\"},"
		"\"reference_videos\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Array of local file paths or http(s) URLs to reference videos for vid2vid. Pass ALL videos in one array, not one at a time. Videos are labeled video#1, video#2, etc. in order\"},"
		"\"duration\":{\"type\":\"integer\",\"description\":\"Video duration in seconds\"}"
		"},\"required\":[\"prompt\"]}",
		vid_gen_exec, ctx, vid_gen_context_destroy);
	if (rc != 0)
		free(ctx);
	else
		tool_set_timeout(reg, "vid_gen", 600);
	return rc;
}
