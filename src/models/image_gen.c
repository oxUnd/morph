#define _GNU_SOURCE
#include "image_gen.h"
#include "models/llm.h"
#include "util/log.h"
#include "util/file.h"
#include "util/base64.h"
#include "util/image_util.h"
#include "util/arena.h"
#include "util/error.h"
#include "util/error.h"
#include "http/client.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>

#include "stb_image.h"

const char *image_gen_ext_from_content_type(const char *headers)
{
	if (!headers) return NULL;
	const char *p = headers;
	while ((p = strcasestr(p, "content-type:")) != NULL) {
		p += 13;
		while (*p == ' ') p++;
		if (strncasecmp(p, "image/png", 9) == 0) return "png";
		if (strncasecmp(p, "image/jpeg", 10) == 0) return "jpg";
		if (strncasecmp(p, "image/webp", 10) == 0) return "webp";
		if (strncasecmp(p, "image/gif", 9) == 0) return "gif";
		if (strncasecmp(p, "image/bmp", 9) == 0) return "bmp";
	}
	return NULL;
}

const char *image_gen_ext_from_magic(const unsigned char *data, size_t len)
{
	if (!data || len < 2) return "png";
	if (len >= 8 && data[0] == 0x89 && data[1] == 'P' &&
	    data[2] == 'N' && data[3] == 'G')
		return "png";
	if (data[0] == 0xFF && data[1] == 0xD8)
		return "jpg";
	if (len >= 12 && memcmp(data, "RIFF", 4) == 0 &&
	    memcmp(data + 8, "WEBP", 4) == 0)
		return "webp";
	if (len >= 6 && (memcmp(data, "GIF87a", 6) == 0 ||
			 memcmp(data, "GIF89a", 6) == 0))
		return "gif";
	if (len >= 2 && data[0] == 'B' && data[1] == 'M')
		return "bmp";
	return "png";
}

static int download_url(const char *url, const char *out_dir,
			char *out_path, size_t out_cap)
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
		MORPH_RETURN(MORPH_ERR_API);
	}
	const char *ext = image_gen_ext_from_content_type(morph_buf_cstr(&resp.headers));
	if (!ext)
		ext = image_gen_ext_from_magic((const unsigned char *)resp.body.data,
					       resp.body.len);
	snprintf(out_path, out_cap, "%s/img_%lld.%s",
		 out_dir, (long long)time(NULL), ext);
	rc = file_write_all(out_path, resp.body.data, resp.body.len);
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
		     const char *size, const char *image_path,
		     const char *output_dir,
		     struct image_result *result)
{
	if (!prompt || !result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));

	struct arena *arena = arena_create(8192);
	if (!arena)
		return -ENOMEM;

	const char *api_base = self ? self->api_base : "https://api.openai.com/v1";
	const char *api_key = (self && self->api_key[0]) ? self->api_key : "";
	const char *model_id = (self && self->model_id[0]) ? self->model_id : "dall-e-3";
	const char *img_size = size ? size : "2048x2048";

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

	if (image_path && image_path[0]) {
		char *b64 = image_encode_base64(image_path, 2048);
		if (b64) {
			size_t uri_len = 22 + strlen(b64) + 1;
			char *data_uri = arena_alloc(arena, uri_len);
			if (data_uri) {
				snprintf(data_uri, uri_len, "data:image/png;base64,%s", b64);
				cJSON_AddStringToObject(body_json, "image", data_uri);
			}
			free(b64);
		}
	}

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

	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
	const char *hdrs[] = { auth_header };

	struct http_response resp = {0};
	int rc = http_post_ex(url, body_str, strlen(body_str),
			      "application/json", hdrs, 1, &resp);
	arena_destroy(arena);

	if (rc < 0) {
		log_err("image_gen: HTTP request failed");
		return rc;
	}

	if (resp.status_code != 200) {
		log_err("image_gen: API returned HTTP %d: %s",
			resp.status_code, resp.body.data ? resp.body.data : "");
		http_response_free(&resp);
		MORPH_RETURN(MORPH_ERR_API);
	}

	cJSON *root = cJSON_Parse(resp.body.data);
	http_response_free(&resp);
	if (!root) {
		log_err("image_gen: failed to parse response");
		MORPH_RETURN(MORPH_ERR_PARSE);
	}

	cJSON *data_arr = cJSON_GetObjectItem(root, "data");
	if (!cJSON_IsArray(data_arr) || cJSON_GetArraySize(data_arr) == 0) {
		cJSON_Delete(root);
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	}

	cJSON *first = cJSON_GetArrayItem(data_arr, 0);
	cJSON *img_url = cJSON_GetObjectItem(first, "url");
	if (!cJSON_IsString(img_url) || !img_url->valuestring) {
		cJSON_Delete(root);
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	}

	strncpy(result->url, img_url->valuestring, sizeof(result->url) - 1);

	char *out_dir;
	if (output_dir && output_dir[0])
		out_dir = file_expand_path(output_dir);
	else
		out_dir = file_expand_path("~/.morph/output");
	file_ensure_dir(out_dir);
	char out_path[PATH_MAX];
	rc = download_url(result->url, out_dir, out_path, sizeof(out_path));
	free(out_dir);
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
		stbi_image_free(img);
	}
	log_dbg("image generated: %s (%dx%d)", out_path, result->width, result->height);
	cJSON_Delete(root);
	return 0;
}
