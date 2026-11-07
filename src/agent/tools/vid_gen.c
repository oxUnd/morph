#include "vid_gen.h"
#include "agent/tool_runtime.h"
#include "models/llm.h"
#include "agent/tool_context.h"
#include "models/video_gen.h"
#include "render/video.h"
#include "util/log.h"
#include "util/error.h"
#include "util/json.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <limits.h>

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

static int is_supported_audio_path(const char *path)
{
	const char *ext;

	if (is_http_url(path))
		return 1;
	ext = path ? strrchr(path, '.') : NULL;
	return ext && (strcasecmp(ext, ".mp3") == 0 ||
		       strcasecmp(ext, ".wav") == 0);
}

static int vid_gen_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct vid_gen_context local = {0};
	struct model request_model;

	if (user_data)
		local = *(struct vid_gen_context *)user_data;
	local.video_llm = model_bind_http_request(
		local.video_llm, result && result->runtime ? &result->runtime->http : NULL,
		&request_model);
	struct vid_gen_context *ctx = user_data ? &local : NULL;
	struct tool_context *tctx = ctx ? ctx->tctx : NULL;

	if (!result)
		return -EINVAL;

	struct json *root = json_parse_prefix(args_json, NULL, NULL);
	const char *prompt = NULL;
	const char *image_paths[VIDEO_GEN_MAX_REFERENCE_IMAGES];
	int num_images = 0;
	const char *video_paths[VIDEO_GEN_MAX_REFERENCE_VIDEOS];
	int num_videos = 0;
	const char *audio_paths[VIDEO_GEN_MAX_REFERENCE_AUDIOS];
	int num_audios = 0;
	int generate_audio = -1;
	int duration = 5;

	memset(image_paths, 0, sizeof(image_paths));
	memset(video_paths, 0, sizeof(video_paths));
	memset(audio_paths, 0, sizeof(audio_paths));

	if (root) {
		struct json *p = json_member_ci(root, "prompt");
		if (json_is_string_value(p))
			prompt = json_string_value(p);

		struct json *ri = json_member_ci(root, "reference_images");
		if (json_is_string_value(ri)) {
			image_paths[0] = json_string_value(ri);
			num_images = 1;
		} else if (json_is_array_value(ri)) {
			int n = json_count(ri);
			if (n > VIDEO_GEN_MAX_REFERENCE_IMAGES) {
				json_free(root);
				(void)tool_result_success_json_text(result, strdup(
					"{\"error\":\"too many reference images\"}"));
				return -EINVAL;
			}
			for (int i = 0; i < n; i++) {
				struct json *item = json_at(ri, i);
				if (json_is_string_value(item)) {
					image_paths[num_images++] = json_string_value(item);
				}
			}
		}

		struct json *rv = json_member_ci(root, "reference_videos");
		if (json_is_string_value(rv)) {
			video_paths[0] = json_string_value(rv);
			num_videos = 1;
		} else if (json_is_array_value(rv)) {
			int n = json_count(rv);
			if (n > VIDEO_GEN_MAX_REFERENCE_VIDEOS) {
				json_free(root);
				(void)tool_result_success_json_text(result, strdup(
					"{\"error\":\"too many reference videos\"}"));
				return -EINVAL;
			}
			for (int i = 0; i < n; i++) {
				struct json *item = json_at(rv, i);
				if (json_is_string_value(item)) {
					video_paths[num_videos++] = json_string_value(item);
				}
			}
		}

		struct json *ra = json_member_ci(root, "reference_audios");
		if (json_is_string_value(ra)) {
			audio_paths[0] = json_string_value(ra);
			num_audios = 1;
		} else if (json_is_array_value(ra)) {
			int n = json_count(ra);
			if (n > VIDEO_GEN_MAX_REFERENCE_AUDIOS) {
				json_free(root);
				(void)tool_result_success_json_text(result, strdup(
					"{\"error\":\"too many reference audios\"}"));
				return -EINVAL;
			}
			for (int i = 0; i < n; i++) {
				struct json *audio = json_at(ra, i);
				if (json_is_string_value(audio))
					audio_paths[num_audios++] = json_string_value(audio);
			}
		}

		struct json *ga = json_member_ci(root, "generate_audio");
		if (json_is_bool_value(ga))
			generate_audio = json_is_true(ga) ? 1 : 0;

		struct json *d = json_member_ci(root, "duration");
		if (json_is_number_value(d))
			duration = (int)json_number_value(d);
	}

	if (!prompt) {
		json_free(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"missing 'prompt' parameter. "
			"Usage: vid_gen({\\\"prompt\\\": \\\"a flying drone\\\", "
			"\\\"reference_images\\\": [\\\"img1.jpg\\\", \\\"img2.png\\\"], "
			"\\\"reference_videos\\\": [\\\"clip1.mp4\\\"], "
			"\\\"reference_audios\\\": [\\\"music.mp3\\\"]})\"}"));
		return -EINVAL;
	}

	char resolved_images[VIDEO_GEN_MAX_REFERENCE_IMAGES][PATH_MAX];
	char resolved_videos[VIDEO_GEN_MAX_REFERENCE_VIDEOS][PATH_MAX];
	char resolved_audios[VIDEO_GEN_MAX_REFERENCE_AUDIOS][PATH_MAX];
	const char *image_paths_to_send[VIDEO_GEN_MAX_REFERENCE_IMAGES];
	const char *video_paths_to_send[VIDEO_GEN_MAX_REFERENCE_VIDEOS];
	const char *audio_paths_to_send[VIDEO_GEN_MAX_REFERENCE_AUDIOS];

	for (int i = 0; i < num_images; i++) {
		image_paths_to_send[i] = image_paths[i];
		if (tctx && image_paths[i] && image_paths[i][0]) {
			int rc = tool_context_authorize_path(
				tctx, TOOL_PATH_READ, image_paths[i],
				resolved_images[i], sizeof(resolved_images[i]));
			if (rc < 0) {
				json_free(root);
				if (rc == -ENOENT)
					(void)tool_result_success_json_text(result, strdup(
						"{\"error\":\"reference image not found\"}"));
				else
					(void)tool_result_success_json_text(result, strdup(
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
				json_free(root);
				if (rc == -ENOENT)
					(void)tool_result_success_json_text(result, strdup(
						"{\"error\":\"reference video not found\"}"));
				else
					(void)tool_result_success_json_text(result, strdup(
						"{\"error\":\"read path outside workspace: permission denied\"}"));
				return rc;
			}
			video_paths_to_send[i] = resolved_videos[i];
		}
	}

	for (int i = 0; i < num_audios; i++) {
		audio_paths_to_send[i] = audio_paths[i];
		if (!is_supported_audio_path(audio_paths[i])) {
			json_free(root);
			(void)tool_result_success_json_text(result, strdup(
				"{\"error\":\"reference audio must be MP3 or WAV\"}"));
			return -EINVAL;
		}
		if (tctx && !is_http_url(audio_paths[i])) {
			int auth_rc = tool_context_authorize_path(
				tctx, TOOL_PATH_READ, audio_paths[i],
				resolved_audios[i], sizeof(resolved_audios[i]));
			if (auth_rc < 0) {
				json_free(root);
				if (auth_rc == -ENOENT)
					(void)tool_result_success_json_text(result, strdup(
						"{\"error\":\"reference audio not found\"}"));
				else
					(void)tool_result_success_json_text(result, strdup(
						"{\"error\":\"read path outside workspace: permission denied\"}"));
				return auth_rc;
			}
			audio_paths_to_send[i] = resolved_audios[i];
		}
	}

	const char *output_dir = tctx ? tool_context_output_dir(tctx) : NULL;
	struct video_result vid_res = {0};
	int rc = video_gen_create(ctx ? ctx->video_llm : NULL, prompt,
				  num_images > 0 ? image_paths_to_send : NULL,
				  num_images,
				  num_videos > 0 ? video_paths_to_send : NULL,
				  num_videos,
				  num_audios > 0 ? audio_paths_to_send : NULL,
				  num_audios, generate_audio,
				  duration, output_dir, &vid_res);
	json_free(root);

	if (rc < 0) {
		if (vid_res.error_msg[0]) {
			char *buf = malloc(strlen(vid_res.error_msg) + 64);
			if (buf) {
				snprintf(buf, strlen(vid_res.error_msg) + 64,
					 "{\"error\":\"%s\"}", vid_res.error_msg);
				(void)tool_result_success_json_text(result, buf);
			} else {
				(void)tool_result_success_json_text(result, strdup("{\"error\":\"video generation failed\"}"));
			}
		} else {
			char err_buf[256];
			snprintf(err_buf, sizeof(err_buf),
				 "{\"error\":\"video generation failed: %s\"}",
				 morph_strerror(rc));
			(void)tool_result_success_json_text(result, strdup(err_buf));
		}
		return rc;
	}

	size_t msg_len = strlen(vid_res.path) + 64;
	char *msg = malloc(msg_len);
	if (!msg)
		return -ENOMEM;
	snprintf(msg, msg_len, "video generated: %s (%ds)",
		 vid_res.path, vid_res.duration_seconds);
	(void)tool_result_success_json_text(result, msg);
	(void)tool_result_add_video(result, vid_res.path,
				    vid_res.duration_seconds);
	log_dbg(result && result->runtime ? result->runtime->http.logger : NULL, "vid_gen: %s",
		msg);

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

	const char *description =
		"Generate a video from a text prompt with optional reference "
		"images, videos, and audios. IMPORTANT: Always pass ALL reference "
		"assets in a single call via the reference_images, reference_videos, "
		"and reference_audios arrays. Never call vid_gen multiple times for "
		"the same video. Assets are labeled image#1, video#1, audio#1, etc., "
		"in order. Use these labels in the prompt to reference specific "
		"assets. Reference videos and audios require Seedance 2.0+.";
	const char *input_schema =
		"{\"type\":\"object\",\"properties\":{"
		"\"prompt\":{\"type\":\"string\",\"description\":\"Text description "
		"of the video. Use image#1, video#1, audio#1, etc. to reference "
		"assets\"},"
		"\"reference_images\":{\"type\":\"array\",\"items\":{\"type\":"
		"\"string\"},\"description\":\"Ordered local reference image paths\"},"
		"\"reference_videos\":{\"type\":\"array\",\"items\":{\"type\":"
		"\"string\"},\"description\":\"Ordered local paths or HTTP(S) URLs "
		"for reference videos\"},"
		"\"reference_audios\":{\"type\":\"array\",\"maxItems\":3,\"items\":"
		"{\"type\":\"string\"},\"description\":\"Ordered local MP3/WAV paths "
		"or HTTP(S) URLs; requires a reference image or video\"},"
		"\"generate_audio\":{\"type\":\"boolean\",\"description\":\"Generate "
		"synchronized audio; defaults true when supported and is required "
		"with reference_audios\"},"
		"\"duration\":{\"type\":\"integer\",\"description\":\"Video duration "
		"in seconds\"}},\"required\":[\"prompt\"]}";
	struct tool_spec spec = {
		.origin = TOOL_ORIGIN_BUILTIN,
		.flags = TOOL_FLAG_MEDIA,
		.name = "vid_gen",
		.description = description,
		.input_schema = input_schema,
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.exec = vid_gen_exec,
		.user_data = ctx,
		.user_data_destroy = vid_gen_context_destroy,
	};
	int rc = tool_register(reg, &spec);
	if (rc != 0)
		free(ctx);
	else
		tool_set_timeout(reg, "vid_gen", 600);
	return rc;
}
