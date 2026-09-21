#include "video_gen.h"
#include "video_provider.h"
#include "models/llm.h"
#include "util/log.h"
#include "util/file.h"
#include "util/base64.h"
#include "util/image_util.h"
#include "util/arena.h"
#include "util/json.h"
#include "util/buf.h"
#include "util/error.h"
#include "http/client.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define VIDEO_VOLCENGINE_ADAPTER "volcengine-videos"

#define VIDEO_REFERENCE_MAX_ENCODED_BYTES (128U * 1024U * 1024U)

static const char *video_reference_mime(const char *path, int audio)
{
	const char *ext = strrchr(path, '.');

	if (audio)
		return ext && !strcasecmp(ext, ".wav") ? "audio/wav" : "audio/mpeg";
	if (ext && !strcasecmp(ext, ".mov"))
		return "video/quicktime";
	if (ext && !strcasecmp(ext, ".webm"))
		return "video/webm";
	if (ext && !strcasecmp(ext, ".mkv"))
		return "video/x-matroska";
	if (ext && !strcasecmp(ext, ".avi"))
		return "video/x-msvideo";
	return "video/mp4";
}

/* Takes ownership of uri on both success and failure. */
static int video_add_reference(cJSON *content, const char *type,
			       const char *role, char *uri, size_t *budget)
{
	cJSON *item = cJSON_CreateObject();
	cJSON *url = item ? cJSON_AddObjectToObject(item, type) : NULL;
	cJSON *value;
	size_t len = strlen(uri);

	if (len > *budget) {
		free(uri);
		cJSON_Delete(item);
		MORPH_RETURN(-EFBIG);
	}
	value = morph_json_take_string(uri);
	if (!value) {
		cJSON_Delete(item);
		MORPH_RETURN(-ENOMEM);
	}
	if (!url || !cJSON_AddItemToObject(url, "url", value)) {
		cJSON_Delete(value);
		cJSON_Delete(item);
		MORPH_RETURN(-ENOMEM);
	}
	if (!cJSON_AddStringToObject(item, "type", type) ||
	    !cJSON_AddStringToObject(item, "role", role) ||
	    !cJSON_AddItemToArray(content, item)) {
		cJSON_Delete(item);
		MORPH_RETURN(-ENOMEM);
	}
	*budget -= len;
	return 0;
}

static int video_reference_uri(const char *path, int audio, size_t budget,
			       char **uri)
{
	morph_buf_t prefix;
	size_t max_bytes = budget / 4 * 3;
	int rc;

	if (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0) {
		*uri = strdup(path);
		if (!*uri)
			MORPH_RETURN(-ENOMEM);
		return 0;
	}
	if (max_bytes > MORPH_MEDIA_MAX_FILE_BYTES)
		max_bytes = MORPH_MEDIA_MAX_FILE_BYTES;
	rc = morph_buf_init(&prefix, 64);
	if (rc != 0)
		MORPH_RETURN(rc);
	rc = morph_buf_printf(&prefix, "data:%s;base64,",
		video_reference_mime(path, audio));
	if (rc == 0)
		rc = base64_encode_file_prefixed(path, morph_buf_cstr(&prefix),
			max_bytes, uri);
	morph_buf_cleanup(&prefix);
	MORPH_RETURN(rc);
}

static const struct video_provider_ops volcengine_video_provider;

static int seedance_version(const char *model_id, int *major, int *minor)
{
	const char *prefix = "doubao-seedance-";
	const char *version;
	int consumed = 0;

	if (!model_id || !major || !minor ||
	    strncmp(model_id, prefix, strlen(prefix)) != 0)
		return 0;
	version = model_id + strlen(prefix);
	if (sscanf(version, "%d-%d%n",
		   major, minor, &consumed) != 2 || consumed <= 0)
		return 0;
	if (*major < 0 || *minor < 0 ||
	    (version[consumed] != '\0' && version[consumed] != '-'))
		return 0;
	return 1;
}

int video_gen_adapter_supported(const char *provider, const char *adapter)
{
	const char *name;

	if (!provider)
		return 0;
	name = adapter && adapter[0] ? adapter : provider;
	if (strcmp(name, VIDEO_VOLCENGINE_ADAPTER) == 0)
		return 1;
	return (!adapter || !adapter[0]) && strcmp(name, "volcengine") == 0;
}

static const struct video_provider_ops *video_provider_resolve(
	const struct model *model)
{
	if (!model || !video_gen_adapter_supported(model->provider,
						   model->adapter))
		return NULL;
	return &volcengine_video_provider;
}

const char *video_gen_adapter_name(const struct model *model)
{
	const struct video_provider_ops *ops = video_provider_resolve(model);

	return ops ? ops->name : NULL;
}

static int volcengine_video_capabilities(const struct model *model,
					 struct video_capabilities *caps)
{
	const char *model_id;
	int major = 0;
	int minor = 0;

	if (!model || !caps)
		MORPH_RETURN(-EINVAL);
	memset(caps, 0, sizeof(*caps));
	caps->supports_generate = 1;
	caps->supports_reference_images = 1;
	if (!model->model_id[0])
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	model_id = model->model_id;
	if (!seedance_version(model_id, &major, &minor))
		return 0;
	if (major > 1 || (major == 1 && minor >= 5))
		caps->supports_generate_audio = 1;
	if (major >= 2) {
		caps->supports_multi_reference_images = 1;
		caps->supports_reference_videos = 1;
		caps->supports_reference_audios = 1;
	}
	return 0;
}

int video_gen_capabilities(const struct model *model,
			   struct video_capabilities *caps)
{
	const struct video_provider_ops *ops;

	if (!model || !caps)
		MORPH_RETURN(-EINVAL);
	ops = video_provider_resolve(model);
	if (!ops)
		MORPH_RETURN(-ENOTSUP);
	return ops->capabilities(model, caps);
}

static int download_url(const char *url, const char *out_path)
{
	struct http_response resp = {0};
	int rc = http_get(url, &resp);
	if (rc < 0) {
		log_err("video download failed: %s", url);
		return rc;
	}
	if (resp.status_code != 200) {
		log_err("video download returned HTTP %d", resp.status_code);
		http_response_free(&resp);
		MORPH_RETURN(MORPH_ERR_API);
	}
	rc = file_write_all(out_path, resp.body.data, resp.body.len);
	http_response_free(&resp);
	return rc;
}

static void video_report_usage(struct model *self, const char *model_id,
			       const char *task_id,
			       const struct video_result *result)
{
	struct model_usage usage;

	memset(&usage, 0, sizeof(usage));
	snprintf(usage.provider, sizeof(usage.provider), "%s",
		 self ? self->provider : "volcengine");
	snprintf(usage.model, sizeof(usage.model), "%s", model_id);
	snprintf(usage.kind, sizeof(usage.kind), "model_video");
	snprintf(usage.usage_source, sizeof(usage.usage_source), "estimated");
	if (task_id && task_id[0])
		snprintf(usage.response_id, sizeof(usage.response_id),
			 "%s", task_id);
	usage.video_seconds = result && result->duration_seconds > 0 ?
		result->duration_seconds : 1;
	model_report_usage(&usage);
}

static int volcengine_video_execute(struct model *self, const char *prompt,
				    const char **image_paths, int num_images,
				    const char **video_paths, int num_videos,
				    const char **audio_paths, int num_audios,
				    int generate_audio,
				    const struct video_capabilities *caps,
				    int duration, const char *output_dir,
				    struct video_result *result)
{
	memset(result, 0, sizeof(*result));

	struct arena *arena = arena_create(8192);
	if (!arena)
		return -ENOMEM;

	const char *api_base = self ? self->api_base : "";
	const char *api_key = (self && self->api_key[0]) ? self->api_key : "";
	const char *model_id = self ? self->model_id : "";
	int poll_interval = self && self->poll_interval_seconds > 0 ?
		self->poll_interval_seconds : 5;
	int poll_timeout = self && self->poll_timeout_seconds > 0 ?
		self->poll_timeout_seconds : 600;

	if (!api_key[0]) {
		snprintf(result->error_msg, sizeof(result->error_msg),
			 "video_gen: no api_key configured");
		log_err("%s", result->error_msg);
		arena_destroy(arena);
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	}

	if (!api_base[0]) {
		snprintf(result->error_msg, sizeof(result->error_msg),
			 "video_gen: no api_base configured");
		log_err("%s", result->error_msg);
		arena_destroy(arena);
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	}

	char submit_url[512];
	snprintf(submit_url, sizeof(submit_url),
		 "%s/contents/generations/tasks", api_base);

	cJSON *body_json = cJSON_CreateObject();
	cJSON_AddStringToObject(body_json, "model", model_id);

	cJSON *content_arr = cJSON_AddArrayToObject(body_json, "content");

	morph_buf_t prompt_buf;
	int prompt_rc = morph_buf_init_arena(&prompt_buf, arena, 256);
	const int counts[] = {num_images > 1 ? num_images : 0, num_videos, num_audios};
	const char *labels[] = {"image", "video", "audio"};
	if (prompt_rc == 0)
		prompt_rc = morph_buf_puts(&prompt_buf, prompt);
	for (size_t kind = 0; prompt_rc == 0 && kind < 3; kind++) {
		if (counts[kind] <= 0)
			continue;
		prompt_rc = morph_buf_printf(&prompt_buf, "\n[Ref %ss: ", labels[kind]);
		for (int i = 0; prompt_rc == 0 && i < counts[kind]; i++)
			prompt_rc = morph_buf_printf(&prompt_buf, "%s%s#%d",
				i ? ", " : "", labels[kind], i + 1);
		if (prompt_rc == 0)
			prompt_rc = morph_buf_putc(&prompt_buf, ']');
	}
	cJSON *item = cJSON_CreateObject();
	if (prompt_rc != 0 || !content_arr || !item ||
	    !cJSON_AddStringToObject(item, "type", "text") ||
	    !cJSON_AddStringToObject(item, "text", morph_buf_cstr(&prompt_buf)) ||
	    !cJSON_AddItemToArray(content_arr, item)) {
		cJSON_Delete(item);
		cJSON_Delete(body_json);
		arena_destroy(arena);
		MORPH_RETURN(prompt_rc != 0 ? prompt_rc : -ENOMEM);
	}

	size_t reference_budget = VIDEO_REFERENCE_MAX_ENCODED_BYTES;
	for (int i = 0; i < num_images; i++) {
		char *uri = NULL;
		int rc;

		if (!image_paths || !image_paths[i] || !image_paths[i][0])
			continue;
		rc = image_encode_data_uri(image_paths[i], 1024, &uri);
		if (rc == 0)
			rc = video_add_reference(content_arr, "image_url",
				caps->supports_multi_reference_images ?
				"reference_image" : "first_frame", uri, &reference_budget);
		if (rc != 0) {
			cJSON_Delete(body_json);
			arena_destroy(arena);
			snprintf(result->error_msg, sizeof(result->error_msg),
				"video_gen: image reference: %s", morph_strerror(rc));
			MORPH_RETURN(rc);
		}
	}

	for (int audio = 0; audio <= 1; audio++) {
		const char **paths = audio ? audio_paths : video_paths;
		int count = audio ? num_audios : num_videos;

		for (int i = 0; i < count; i++) {
			char *uri = NULL;
			int rc;

			if (!paths || !paths[i] || !paths[i][0])
				continue;
			rc = video_reference_uri(paths[i], audio, reference_budget, &uri);
			if (rc == 0)
				rc = video_add_reference(content_arr,
					audio ? "audio_url" : "video_url",
					audio ? "reference_audio" : "reference_video",
					uri, &reference_budget);
			if (rc != 0) {
				cJSON_Delete(body_json);
				arena_destroy(arena);
				snprintf(result->error_msg, sizeof(result->error_msg),
					"video_gen: media reference: %s", morph_strerror(rc));
				MORPH_RETURN(rc);
			}
		}
	}

	if (duration > 0)
		cJSON_AddNumberToObject(body_json, "duration", duration);
	if (generate_audio >= 0)
		cJSON_AddBoolToObject(body_json, "generate_audio",
				      generate_audio != 0);

	char *body_str = morph_json_print(arena, body_json);
	cJSON_Delete(body_json);

	if (!body_str) {
		arena_destroy(arena);
		return -ENOMEM;
	}

	log_info("video_gen: request body (%zu bytes)", strlen(body_str));
	log_dbg("video_gen: request body:\n%s", body_str);

	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header),
		 "Authorization: Bearer %s", api_key);
	const char *hdrs[] = { auth_header };

	struct http_response resp = {0};
	int rc = http_post_ex(submit_url, body_str, strlen(body_str),
			      "application/json", hdrs, 1, &resp);
	arena_reset(arena);

	if (rc < 0) {
		snprintf(result->error_msg, sizeof(result->error_msg),
			 "video_gen: submit request failed (curl error %d)", rc);
		log_err("%s", result->error_msg);
		arena_destroy(arena);
		return rc;
	}

	char task_id[128] = {0};
	char task_status[32] = {0};
	char video_url[2048] = {0};

	if (resp.status_code == 200 && resp.body.data) {
		log_dbg("video_gen: submit response (%d):\n%s",
			resp.status_code, resp.body.data);
		cJSON *root = cJSON_Parse(resp.body.data);
		if (root) {
			cJSON *id_item = cJSON_GetObjectItem(root, "id");
			if (cJSON_IsString(id_item) && id_item->valuestring)
				strncpy(task_id, id_item->valuestring,
					sizeof(task_id) - 1);
			cJSON *status_item = cJSON_GetObjectItem(root, "status");
			if (cJSON_IsString(status_item) && status_item->valuestring)
				strncpy(task_status, status_item->valuestring,
					sizeof(task_status) - 1);
			cJSON *content = cJSON_GetObjectItem(root, "content");
			if (cJSON_IsObject(content)) {
				cJSON *vurl = cJSON_GetObjectItem(content, "video_url");
				if (cJSON_IsString(vurl) && vurl->valuestring)
					strncpy(video_url, vurl->valuestring,
						sizeof(video_url) - 1);
			}
			cJSON_Delete(root);
		} else {
			log_err("video_gen: failed to parse submit response: %s",
				resp.body.data);
		}
	} else {
		snprintf(result->error_msg, sizeof(result->error_msg),
			 "video_gen: submit HTTP %d, body: %s",
			 resp.status_code,
			 resp.body.data ? resp.body.data : "(empty)");
		log_err("%s", result->error_msg);
	}
	http_response_free(&resp);

	if (!task_id[0]) {
		if (!result->error_msg[0])
			snprintf(result->error_msg, sizeof(result->error_msg),
				 "video_gen: no task id in submit response");
		log_err("%s", result->error_msg);
		arena_destroy(arena);
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	}
	if (task_status[0])
		log_info("video_gen: task submitted: %s (status=%s)", task_id, task_status);
	else
		log_info("video_gen: task submitted: %s", task_id);

	if (video_url[0]) {
		arena_destroy(arena);
		goto download;
	}

	char query_url[640];
	snprintf(query_url, sizeof(query_url),
		 "%s/contents/generations/tasks/%s",
		 api_base, task_id);

	time_t deadline = time(NULL) + poll_timeout;
	while (time(NULL) < deadline) {
		sleep((unsigned int)poll_interval);

		struct http_response qresp = {0};
		int qrc = http_get_ex(query_url, hdrs, 1, &qresp);

		if (qrc < 0) {
			log_warn("video_gen: query request failed, retrying...");
			continue;
		}

		if (qresp.status_code != 200 || !qresp.body.data) {
			http_response_free(&qresp);
			continue;
		}

		cJSON *qroot = cJSON_Parse(qresp.body.data);
		if (!qroot) {
			http_response_free(&qresp);
			continue;
		}

		cJSON *status_item = cJSON_GetObjectItem(qroot, "status");
		const char *st = cJSON_IsString(status_item)
				 ? status_item->valuestring : "";

		if (strcmp(st, "succeeded") == 0 ||
		    strcmp(st, "completed") == 0) {
			cJSON *vcontent = cJSON_GetObjectItem(qroot, "content");
			if (cJSON_IsObject(vcontent)) {
				cJSON *vurl = cJSON_GetObjectItem(vcontent, "video_url");
				if (cJSON_IsString(vurl) && vurl->valuestring)
					strncpy(video_url, vurl->valuestring,
						sizeof(video_url) - 1);
			}
			cJSON_Delete(qroot);
			http_response_free(&qresp);
			break;
		}

		if (strcmp(st, "failed") == 0) {
			cJSON *err_obj = cJSON_GetObjectItem(qroot, "error");
			const char *err_code = cJSON_IsObject(err_obj)
					       ? cJSON_GetStringValue(cJSON_GetObjectItem(err_obj, "code")) : "";
			const char *err_msg = cJSON_IsObject(err_obj)
					      ? cJSON_GetStringValue(cJSON_GetObjectItem(err_obj, "message")) : "";
			snprintf(result->error_msg, sizeof(result->error_msg),
				 "video_gen: task %s failed: %s %s",
				 task_id, err_code, err_msg);
			log_err("%s", result->error_msg);
			log_dbg("video_gen: poll response body:\n%s", qresp.body.data);
			cJSON_Delete(qroot);
			http_response_free(&qresp);
			arena_destroy(arena);
			MORPH_RETURN(MORPH_ERR_API);
		}

		log_info("video_gen: task %s status=%s", task_id, st);

		cJSON_Delete(qroot);
		http_response_free(&qresp);
	}

	arena_destroy(arena);

	if (!video_url[0]) {
		snprintf(result->error_msg, sizeof(result->error_msg),
			 "video_gen: timed out for task: %s", task_id);
		log_err("%s", result->error_msg);
		return -ETIMEDOUT;
	}

download:
	strncpy(result->url, video_url, sizeof(result->url) - 1);

	char *out_dir;
	if (output_dir && output_dir[0])
		out_dir = file_expand_path(output_dir);
	else
		out_dir = file_expand_path("~/.morph/output");
	file_ensure_dir(out_dir);
	char out_path[PATH_MAX];
	snprintf(out_path, sizeof(out_path), "%s/vid_%lld.mp4",
		 out_dir, (long long)time(NULL));
	free(out_dir);

	rc = download_url(result->url, out_path);
	if (rc < 0)
		return rc;

	strncpy(result->path, out_path, sizeof(result->path) - 1);
	result->duration_seconds = duration > 0 ? duration : 5;
	result->status = 1;

	log_dbg("video generated: %s (%ds)", out_path, result->duration_seconds);
	video_report_usage(self, model_id, task_id, result);
	return 0;
}

static const struct video_provider_ops volcengine_video_provider = {
	.name = VIDEO_VOLCENGINE_ADAPTER,
	.capabilities = volcengine_video_capabilities,
	.execute = volcengine_video_execute,
};

static int video_capability_error(struct model *self,
				  struct video_result *result,
				  const char *capability)
{
	const char *adapter = video_gen_adapter_name(self);
	const char *model_id = self && self->model_id[0] ?
		self->model_id : "(not configured)";

	snprintf(result->error_msg, sizeof(result->error_msg),
		 "video adapter '%s' does not support %s for model '%s'",
		 adapter ? adapter : "(unknown)", capability, model_id);
	if (self)
		snprintf(self->last_error, sizeof(self->last_error), "%s",
			 result->error_msg);
	MORPH_RETURN(-ENOTSUP);
}

int video_gen_create(struct model *self, const char *prompt,
		    const char **image_paths, int num_images,
		    const char **video_paths, int num_videos,
		    const char **audio_paths, int num_audios,
		    int generate_audio,
		    int duration, const char *output_dir,
		    struct video_result *result)
{
	const struct video_provider_ops *ops;
	struct video_capabilities caps;
	int rc;

	if (!self || !prompt || !result || num_images < 0 || num_videos < 0 ||
	    num_audios < 0 || num_images > VIDEO_GEN_MAX_REFERENCE_IMAGES ||
	    num_videos > VIDEO_GEN_MAX_REFERENCE_VIDEOS ||
	    num_audios > VIDEO_GEN_MAX_REFERENCE_AUDIOS ||
	    (num_images > 0 && !image_paths) ||
	    (num_videos > 0 && !video_paths) ||
	    (num_audios > 0 && !audio_paths) ||
	    generate_audio < -1 || generate_audio > 1)
		MORPH_RETURN(-EINVAL);
	memset(result, 0, sizeof(*result));
	if (!self->model_id[0]) {
		snprintf(result->error_msg, sizeof(result->error_msg),
			 "video_gen: no model configured");
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	}
	for (int i = 0; i < num_images; i++)
		if (!image_paths[i] || !image_paths[i][0])
			MORPH_RETURN(-EINVAL);
	for (int i = 0; i < num_videos; i++)
		if (!video_paths[i] || !video_paths[i][0])
			MORPH_RETURN(-EINVAL);
	for (int i = 0; i < num_audios; i++)
		if (!audio_paths[i] || !audio_paths[i][0])
			MORPH_RETURN(-EINVAL);

	ops = video_provider_resolve(self);
	if (!ops) {
		snprintf(result->error_msg, sizeof(result->error_msg),
			 "unsupported video adapter for provider '%s'",
			 self->provider);
		MORPH_RETURN(-ENOTSUP);
	}
	rc = ops->capabilities(self, &caps);
	if (rc != 0) {
		snprintf(result->error_msg, sizeof(result->error_msg),
			 "unsupported video adapter for provider '%s'",
			 self->provider);
		MORPH_RETURN(rc);
	}
	if (num_images > 0 && !caps.supports_reference_images)
		return video_capability_error(self, result, "reference images");
	if (num_images > 1 && !caps.supports_multi_reference_images)
		return video_capability_error(self, result,
					      "multiple reference images");
	if (num_videos > 0 && !caps.supports_reference_videos)
		return video_capability_error(self, result, "reference videos");
	if (num_audios > 0 && !caps.supports_reference_audios)
		return video_capability_error(self, result, "reference audios");
	if (num_audios > 0 && num_images == 0 && num_videos == 0) {
		snprintf(result->error_msg, sizeof(result->error_msg),
			 "reference audio requires a reference image or video");
		MORPH_RETURN(-EINVAL);
	}
	if (num_audios > 0 && generate_audio == 0) {
		snprintf(result->error_msg, sizeof(result->error_msg),
			 "reference audio requires generate_audio=true");
		MORPH_RETURN(-EINVAL);
	}
	if (generate_audio == 1 && !caps.supports_generate_audio)
		return video_capability_error(self, result, "audio generation");
	if (num_audios > 0)
		generate_audio = 1;
	else if (generate_audio < 0 && caps.supports_generate_audio)
		generate_audio = 1;

	return ops->execute(self, prompt, image_paths, num_images,
			    video_paths, num_videos, audio_paths, num_audios,
			    generate_audio, &caps, duration, output_dir, result);
}
