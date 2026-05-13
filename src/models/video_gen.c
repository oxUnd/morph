#include "video_gen.h"
#include "models/llm.h"
#include "util/log.h"
#include "util/file.h"
#include "http/client.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

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
		return -EIO;
	}
	rc = file_write_all(out_path, resp.body, resp.body_len);
	http_response_free(&resp);
	return rc;
}

int video_gen_create(struct model *self, const char *prompt,
		    const char *image_path, int duration,
		    struct video_result *result)
{
	if (!prompt || !result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));

	const char *api_base = self ? self->api_base : "";
	const char *api_key = (self && self->api_key[0]) ? self->api_key : "";
	const char *model_id = (self && self->model_id[0]) ? self->model_id
			       : "doubao-seedance-1-0-pro-fast-251015";
	int poll_interval = 5;
	int poll_timeout = 600;

	if (!api_base[0]) {
		log_err("video_gen: no api_base configured");
		return -EINVAL;
	}

	char submit_url[512];
	snprintf(submit_url, sizeof(submit_url),
		 "%s/contents/generations/tasks", api_base);

	cJSON *body_json = cJSON_CreateObject();
	cJSON_AddStringToObject(body_json, "model", model_id);

	cJSON *content_arr = cJSON_AddArrayToObject(body_json, "content");
	cJSON *item = cJSON_CreateObject();
	cJSON_AddStringToObject(item, "type", "text");

	char full_prompt[8192];
	if (image_path && image_path[0])
		snprintf(full_prompt, sizeof(full_prompt), "%s\nreference image: %s",
			 prompt, image_path);
	else
		snprintf(full_prompt, sizeof(full_prompt), "%s", prompt);
	cJSON_AddStringToObject(item, "text", full_prompt);
	cJSON_AddItemToArray(content_arr, item);

	if (duration > 0)
		cJSON_AddNumberToObject(body_json, "duration", duration);
	cJSON_AddNumberToObject(body_json, "n", 1);

	char *body_str = cJSON_PrintUnformatted(body_json);
	cJSON_Delete(body_json);

	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header),
		 "Authorization: Bearer %s", api_key);
	const char *hdrs[] = { auth_header };

	struct http_response resp = {0};
	int rc = http_post_ex(submit_url, body_str, strlen(body_str),
			      "application/json", hdrs, 1, &resp);
	free(body_str);

	if (rc < 0) {
		log_err("video_gen: submit request failed");
		return rc;
	}

	char task_id[128] = {0};
	char task_status[32] = {0};
	char video_url[2048] = {0};

	if (resp.status_code == 200 && resp.body) {
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
		}
	}
	http_response_free(&resp);

	if (!task_id[0]) {
		log_err("video_gen: no task id in submit response");
		return -EIO;
	}
	log_info("video_gen: task submitted: %s (status=%s)",
		 task_id, task_status);

	if (video_url[0]) {
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
				log_err("video_gen: task failed: %s", task_id);
				cJSON_Delete(qroot);
				http_response_free(&qresp);
				return -EIO;
			}

			log_info("video_gen: task %s status=%s", task_id, st);
		}

		cJSON_Delete(qroot);
		http_response_free(&qresp);
	}

	if (!video_url[0]) {
		log_err("video_gen: timed out for task: %s", task_id);
		return -ETIMEDOUT;
	}

download:
	strncpy(result->url, video_url, sizeof(result->url) - 1);

	char *out_dir = file_expand_path("~/.multi-agent/output");
	file_ensure_dir(out_dir);
	char out_path[1024];
	snprintf(out_path, sizeof(out_path), "%s/vid_%lld.mp4",
		 out_dir, (long long)time(NULL));
	free(out_dir);

	rc = download_url(result->url, out_path);
	if (rc < 0)
		return rc;

	strncpy(result->path, out_path, sizeof(result->path) - 1);
	result->duration_seconds = duration > 0 ? duration : 5;
	result->status = 1;

	log_info("video generated: %s (%ds)", out_path, result->duration_seconds);
	return 0;
}
