#include "video_gen.h"
#include "models/llm.h"
#include "util/log.h"
#include "util/file.h"
#include "util/base64.h"
#include "util/image_util.h"
#include "util/arena.h"
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
#include "util/error.h"

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
	rc = file_write_all(out_path, resp.body, resp.body_len);
	http_response_free(&resp);
	return rc;
}

int video_gen_create(struct model *self, const char *prompt,
		    const char **image_paths, int num_images,
		    const char **video_paths, int num_videos,
		    int duration, const char *output_dir,
		    struct video_result *result)
{
	if (!prompt || !result)
		return -EINVAL;
	if (num_images < 0)
		num_images = 0;
	if (num_videos < 0)
		num_videos = 0;
	memset(result, 0, sizeof(*result));

	struct arena *arena = arena_create(8192);
	if (!arena)
		return -ENOMEM;

	const char *api_base = self ? self->api_base : "";
	const char *api_key = (self && self->api_key[0]) ? self->api_key : "";
	const char *model_id = (self && self->model_id[0]) ? self->model_id
			       : "doubao-seedance-1-0-pro-fast-251015";
	int poll_interval = 5;
	int poll_timeout = 600;

	if (!api_base[0]) {
		log_err("video_gen: no api_base configured");
		arena_destroy(arena);
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	}

	char submit_url[512];
	snprintf(submit_url, sizeof(submit_url),
		 "%s/contents/generations/tasks", api_base);

	cJSON *body_json = cJSON_CreateObject();
	cJSON_AddStringToObject(body_json, "model", model_id);

	cJSON *content_arr = cJSON_AddArrayToObject(body_json, "content");

	const char *final_prompt = prompt;
	if (num_images > 1 || num_videos > 0) {
		size_t suffix_len = 96 + (size_t)(num_images + num_videos) * 16;
		size_t total = strlen(prompt) + suffix_len;
		char *buf = arena_alloc(arena, total);
		if (buf) {
			size_t off = (size_t)snprintf(buf, total, "%s", prompt);
			if (num_images > 1) {
				off += (size_t)snprintf(buf + off, total - off,
						       "\n[Ref images: ");
				for (int i = 0; i < num_images; i++) {
					if (i > 0)
						off += (size_t)snprintf(buf + off, total - off, ", ");
					off += (size_t)snprintf(buf + off, total - off, "image#%d", i + 1);
				}
				off += (size_t)snprintf(buf + off, total - off, "]");
			}
			if (num_videos > 0) {
				off += (size_t)snprintf(buf + off, total - off,
						       "\n[Ref videos: ");
				for (int i = 0; i < num_videos; i++) {
					if (i > 0)
						off += (size_t)snprintf(buf + off, total - off, ", ");
					off += (size_t)snprintf(buf + off, total - off, "video#%d", i + 1);
				}
				off += (size_t)snprintf(buf + off, total - off, "]");
			}
			final_prompt = buf;
		}
	}

	cJSON *item = cJSON_CreateObject();
	cJSON_AddStringToObject(item, "type", "text");
	cJSON_AddStringToObject(item, "text", final_prompt);
	cJSON_AddItemToArray(content_arr, item);

	for (int i = 0; i < num_images; i++) {
		if (!image_paths || !image_paths[i] || !image_paths[i][0])
			continue;
		char *b64 = image_encode_base64(image_paths[i], 1024);
		if (!b64) {
			log_warn("video_gen: failed to encode image: %s", image_paths[i]);
			continue;
		}
		size_t uri_len = 22 + strlen(b64) + 1;
		char *data_uri = arena_alloc(arena, uri_len);
		if (data_uri) {
			snprintf(data_uri, uri_len, "data:image/png;base64,%s", b64);
			cJSON *img_item = cJSON_CreateObject();
			cJSON_AddStringToObject(img_item, "type", "image_url");
			cJSON *url_obj = cJSON_CreateObject();
			cJSON_AddStringToObject(url_obj, "url", data_uri);
			cJSON_AddItemToObject(img_item, "image_url", url_obj);
			cJSON_AddItemToArray(content_arr, img_item);
		}
		free(b64);
	}

	for (int i = 0; i < num_videos; i++) {
		if (!video_paths || !video_paths[i] || !video_paths[i][0])
			continue;
		const char *vpath = video_paths[i];
		const char *url_to_send = NULL;
		char *b64 = NULL;

		if (strncmp(vpath, "http://", 7) == 0 ||
		    strncmp(vpath, "https://", 8) == 0) {
			url_to_send = vpath;
		} else {
			size_t vlen = 0;
			char *vdata = file_read_all(vpath, &vlen);
			if (!vdata || vlen == 0) {
				log_warn("video_gen: failed to read video: %s", vpath);
				free(vdata);
				continue;
			}
			b64 = base64_encode((unsigned char *)vdata, vlen);
			free(vdata);
			if (!b64) {
				log_warn("video_gen: failed to encode video: %s", vpath);
				continue;
			}
			const char *mime = "video/mp4";
			const char *ext = strrchr(vpath, '.');
			if (ext) {
				if (!strcasecmp(ext, ".mov"))
					mime = "video/quicktime";
				else if (!strcasecmp(ext, ".webm"))
					mime = "video/webm";
				else if (!strcasecmp(ext, ".mkv"))
					mime = "video/x-matroska";
				else if (!strcasecmp(ext, ".avi"))
					mime = "video/x-msvideo";
			}
			size_t uri_len = strlen("data:") + strlen(mime) +
					 strlen(";base64,") + strlen(b64) + 1;
			char *data_uri = arena_alloc(arena, uri_len);
			if (data_uri) {
				snprintf(data_uri, uri_len, "data:%s;base64,%s",
					 mime, b64);
				url_to_send = data_uri;
			}
		}

		if (url_to_send) {
			cJSON *vid_item = cJSON_CreateObject();
			cJSON_AddStringToObject(vid_item, "type", "video_url");
			cJSON *url_obj = cJSON_CreateObject();
			cJSON_AddStringToObject(url_obj, "url", url_to_send);
			cJSON_AddItemToObject(vid_item, "video_url", url_obj);
			cJSON_AddItemToArray(content_arr, vid_item);
		}
		free(b64);
	}

	if (duration > 0)
		cJSON_AddNumberToObject(body_json, "duration", duration);
	cJSON_AddNumberToObject(body_json, "n", 1);

	size_t body_cap = 8192;
	char *body_str = arena_alloc(arena, body_cap);
	while (body_str && !cJSON_PrintPreallocated(body_json, body_str, (int)body_cap, 0)) {
		body_cap *= 2;
		body_str = arena_alloc(arena, body_cap);
	}
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
		log_err("video_gen: submit request failed");
		arena_destroy(arena);
		return rc;
	}

	char task_id[128] = {0};
	char task_status[32] = {0};
	char video_url[2048] = {0};

	if (resp.status_code == 200 && resp.body) {
		log_dbg("video_gen: submit response (%d):\n%s",
			resp.status_code, resp.body);
		cJSON *root = cJSON_Parse(resp.body);
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
				resp.body);
		}
	} else {
		log_err("video_gen: submit HTTP %d, body:\n%s",
			resp.status_code,
			resp.body ? resp.body : "(empty)");
	}
	http_response_free(&resp);

	if (!task_id[0]) {
		log_err("video_gen: no task id in submit response");
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
		 "%s/contents/generations/tasks?id=%s&model=%s",
		 api_base, task_id, model_id);

	time_t deadline = time(NULL) + poll_timeout;
	while (time(NULL) < deadline) {
		sleep((unsigned int)poll_interval);

		struct http_response qresp = {0};
		int qrc = http_get_ex(query_url, hdrs, 1, &qresp);

		if (qrc < 0) {
			log_warn("video_gen: query request failed, retrying...");
			continue;
		}

		if (qresp.status_code != 200 || !qresp.body) {
			http_response_free(&qresp);
			continue;
		}

		cJSON *qroot = cJSON_Parse(qresp.body);
		if (!qroot) {
			http_response_free(&qresp);
			continue;
		}

		cJSON *items = cJSON_GetObjectItem(qroot, "items");
		cJSON *first = cJSON_IsArray(items) && cJSON_GetArraySize(items) > 0
			       ? cJSON_GetArrayItem(items, 0) : NULL;

		if (first) {
			cJSON *status_item = cJSON_GetObjectItem(first, "status");
			const char *st = cJSON_IsString(status_item)
					 ? status_item->valuestring : "";

			if (strcmp(st, "succeeded") == 0 ||
			    strcmp(st, "completed") == 0) {
				cJSON *vcontent = cJSON_GetObjectItem(first, "content");
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
				cJSON *err_obj = cJSON_GetObjectItem(first, "error");
				const char *err_code = cJSON_IsObject(err_obj)
						       ? cJSON_GetStringValue(cJSON_GetObjectItem(err_obj, "code")) : "";
				const char *err_msg = cJSON_IsObject(err_obj)
						      ? cJSON_GetStringValue(cJSON_GetObjectItem(err_obj, "message")) : "";
				log_err("video_gen: task %s failed: %s %s",
					task_id, err_code, err_msg);
				log_dbg("video_gen: poll response body:\n%s", qresp.body);
				cJSON_Delete(qroot);
				http_response_free(&qresp);
				arena_destroy(arena);
				MORPH_RETURN(MORPH_ERR_API);
			}

			log_info("video_gen: task %s status=%s", task_id, st);
		}

		cJSON_Delete(qroot);
		http_response_free(&qresp);
	}

	arena_destroy(arena);

	if (!video_url[0]) {
		log_err("video_gen: timed out for task: %s", task_id);
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
	return 0;
}
