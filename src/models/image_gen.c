#include "image_gen.h"
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

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static int download_url(const char *url, const char *out_path)
{
	struct http_response resp = {0};
	int rc = http_get(url, &resp);
	if (rc < 0) {
		log_err("download failed: %s", url);
		return rc;
	}
	if (resp.status_code != 200) {
		log_err("download returned HTTP %d", resp.status_code);
		http_response_free(&resp);
		return -EIO;
	}
	rc = file_write_all(out_path, resp.body, resp.body_len);
	http_response_free(&resp);
	return rc;
}

static const char *style_prefix(const char *style)
{
	if (!style || !*style)
		return "";
	if (strcmp(style, "realistic") == 0) return "Realistic photography style: ";
	if (strcmp(style, "cartoon") == 0) return "Cartoon/anime style: ";
	if (strcmp(style, "oil-painting") == 0) return "Oil painting style: ";
	if (strcmp(style, "watercolor") == 0) return "Watercolor painting style: ";
	if (strcmp(style, "sketch") == 0) return "Pencil sketch style: ";
	if (strcmp(style, "cyberpunk") == 0) return "Cyberpunk style: ";
	if (strcmp(style, "vintage") == 0) return "Vintage/retro style: ";
	if (strcmp(style, "minimalist") == 0) return "Minimalist flat design: ";
	return "";
}

int image_gen_create(struct model *self, const char *prompt, const char *style,
		     const char *size, struct image_result *result)
{
	if (!prompt || !result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));

	const char *api_base = self ? self->api_base : "https://api.openai.com/v1";
	const char *api_key = (self && self->api_key[0]) ? self->api_key : "";
	const char *model_id = (self && self->model_id[0]) ? self->model_id : "dall-e-3";
	const char *img_size = size ? size : "1024x1024";

	char url[512];
	snprintf(url, sizeof(url), "%s/images/generations", api_base);

	char enhanced_prompt[4096];
	const char *prefix = style_prefix(style);
	snprintf(enhanced_prompt, sizeof(enhanced_prompt), "%s%s", prefix, prompt);

	cJSON *body_json = cJSON_CreateObject();
	cJSON_AddStringToObject(body_json, "model", model_id);
	cJSON_AddStringToObject(body_json, "prompt", enhanced_prompt);
	cJSON_AddNumberToObject(body_json, "n", 1);
	cJSON_AddStringToObject(body_json, "size", img_size);
	cJSON_AddStringToObject(body_json, "response_format", "url");
	char *body_str = cJSON_PrintUnformatted(body_json);
	cJSON_Delete(body_json);

	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
	const char *hdrs[] = { auth_header };

	struct http_response resp = {0};
	int rc = http_post_ex(url, body_str, strlen(body_str),
			      "application/json", hdrs, 1, &resp);
	free(body_str);

	if (rc < 0) {
		log_err("image_gen: HTTP request failed");
		return rc;
	}

	if (resp.status_code != 200) {
		log_err("image_gen: API returned HTTP %d: %s",
			resp.status_code, resp.body ? resp.body : "");
		http_response_free(&resp);
		return -EIO;
	}

	cJSON *root = cJSON_Parse(resp.body);
	http_response_free(&resp);
	if (!root) {
		log_err("image_gen: failed to parse response");
		return -EIO;
	}

	cJSON *data_arr = cJSON_GetObjectItem(root, "data");
	if (!cJSON_IsArray(data_arr) || cJSON_GetArraySize(data_arr) == 0) {
		cJSON_Delete(root);
		return -EIO;
	}

	cJSON *first = cJSON_GetArrayItem(data_arr, 0);
	cJSON *img_url = cJSON_GetObjectItem(first, "url");
	if (!cJSON_IsString(img_url) || !img_url->valuestring) {
		cJSON_Delete(root);
		return -EIO;
	}

	strncpy(result->url, img_url->valuestring, sizeof(result->url) - 1);

	char *out_dir = file_expand_path("~/.multi-agent/output");
	file_ensure_dir(out_dir);
	char out_path[1024];
	snprintf(out_path, sizeof(out_path), "%s/img_%lld.png",
		 out_dir, (long long)time(NULL));
	free(out_dir);

	rc = download_url(result->url, out_path);
	if (rc < 0) {
		cJSON_Delete(root);
		return rc;
	}
	strncpy(result->path, out_path, sizeof(result->path) - 1);

	int w = 0, h = 0, ch = 0;
	unsigned char *img = stbi_load(out_path, &w, &h, &ch, 0);
	if (img) {
		result->width = w;
		result->height = h;
		STBI_FREE(img);
	}
	log_info("image generated: %s (%dx%d)", out_path, result->width, result->height);
	cJSON_Delete(root);
	return 0;
}