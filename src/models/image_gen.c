#define _GNU_SOURCE
#include "image_gen.h"
#include "image_provider.h"
#include "media_output.h"
#include "models/llm.h"
#include "util/log.h"
#include "util/file.h"
#include "util/base64.h"
#include "util/image_util.h"
#include "util/cleanup.h"
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
#include <limits.h>

#include "stb_image.h"

int image_provider_build_request_meta(const struct model *model, const char *path, morph_buf_t *url,
				      morph_buf_t *auth)
{
	if (!model || !path || !url || !auth || url == auth)
		return -EINVAL;
	if (url->data || auth->data)
		return -EEXIST;
	morph_buf_t url_owner MORPH_CLEANUP(morph_buf_cleanup) = {0};
	morph_buf_t auth_owner MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_buf_init(&url_owner, strlen(model->api_base) + strlen(path) + 1);
	if (rc == 0)
		rc = morph_buf_printf(&url_owner, "%s%s", model->api_base, path);
	if (rc == 0)
		rc = model_authorization_header_create(model, &auth_owner);
	if (rc != 0)
		return rc;
	morph_buf_move(url, &url_owner);
	morph_buf_move(auth, &auth_owner);
	return 0;
}

int image_provider_post_json(struct model *model, const char *path, const char *label,
			     const char *body, struct image_payload *payload)
{
	struct http_response response = {0};
	morph_buf_t url = {0};
	morph_buf_t auth = {0};
	const char *headers[1];
	int rc;

	if (!label || !body || !payload)
		return -EINVAL;
	if (payload->data)
		return -EEXIST;
	rc = image_provider_build_request_meta(model, path, &url, &auth);
	if (rc != 0)
		return rc;
	headers[0] = morph_buf_cstr_borrow(&auth);
	rc = http_post_ex_timeout(&model->http, morph_buf_cstr_borrow(&url), body, strlen(body),
				  "application/json", headers, 1, model->timeout_seconds,
				  &response);
	morph_buf_cleanup(&auth);
	morph_buf_cleanup(&url);
	if (rc == 0) {
		if (response.status_code != 200) {
			snprintf(model->last_error, sizeof(model->last_error),
				 "%s returned HTTP %d: %.400s", label, response.status_code,
				 response.body.data ? response.body.data : "");
			rc = MORPH_ERR_API;
		} else {
			rc = image_provider_parse_response(model, response.body.data, payload);
		}
	}
	http_response_free(&response);
	return rc;
}

const char *image_gen_ext_from_content_type(const char *headers)
{
	if (!headers)
		return NULL;
	const char *p = headers;
	while ((p = strcasestr(p, "content-type:")) != NULL) {
		p += 13;
		while (*p == ' ')
			p++;
		if (strncasecmp(p, "image/png", 9) == 0)
			return "png";
		if (strncasecmp(p, "image/jpeg", 10) == 0)
			return "jpg";
		if (strncasecmp(p, "image/webp", 10) == 0)
			return "webp";
		if (strncasecmp(p, "image/gif", 9) == 0)
			return "gif";
		if (strncasecmp(p, "image/bmp", 9) == 0)
			return "bmp";
	}
	return NULL;
}

const char *image_gen_ext_from_magic(const unsigned char *data, size_t len)
{
	if (!data || len < 2)
		return "png";
	if (len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
		return "png";
	if (data[0] == 0xFF && data[1] == 0xD8)
		return "jpg";
	if (len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0)
		return "webp";
	if (len >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0))
		return "gif";
	if (len >= 2 && data[0] == 'B' && data[1] == 'M')
		return "bmp";
	return "png";
}

static int download_url(const struct http_control *control, const char *url, const char *out_dir,
			char *out_path, size_t out_cap)
{
	struct http_response resp = {0};
	int rc = http_get(control, url, &resp);
	if (rc < 0) {
		http_response_free(&resp);
		log_err(control ? control->logger : NULL, "download failed: %s", url);
		return rc;
	}
	if (resp.status_code != 200) {
		log_err(control ? control->logger : NULL, "download returned HTTP %d",
			resp.status_code);
		http_response_free(&resp);
		return MORPH_ERR_API;
	}
	const char *ext = image_gen_ext_from_content_type(morph_buf_cstr_borrow(&resp.headers));
	if (!ext)
		ext = image_gen_ext_from_magic((const unsigned char *)resp.body.data,
					       resp.body.len);
	rc = media_output_write(out_dir, "img", ext, resp.body.data, resp.body.len, out_path,
				out_cap);
	http_response_free(&resp);
	return rc;
}

static const char *style_prefix(const char *style)
{
	if (!style || !*style)
		return "";
	if (strcmp(style, "realistic") == 0)
		return "Realistic photography style: ";
	if (strcmp(style, "cartoon") == 0)
		return "Cartoon/anime style: ";
	if (strcmp(style, "oil-painting") == 0)
		return "Oil painting style: ";
	if (strcmp(style, "watercolor") == 0)
		return "Watercolor painting style: ";
	if (strcmp(style, "sketch") == 0)
		return "Pencil sketch style: ";
	if (strcmp(style, "cyberpunk") == 0)
		return "Cyberpunk style: ";
	if (strcmp(style, "vintage") == 0)
		return "Vintage/retro style: ";
	if (strcmp(style, "minimalist") == 0)
		return "Minimalist flat design: ";
	return "";
}

int image_gen_validate_size(const char *size)
{
	const long long min_pixels = 2560LL * 1440LL;
	const long long max_pixels = 4096LL * 4096LL;
	char *end = NULL;
	long width;
	long height;
	long long pixels;

	if (!size || !*size)
		return 0;

	if (strcmp(size, "2k") == 0 || strcmp(size, "4k") == 0)
		return 0;

	errno = 0;
	width = strtol(size, &end, 10);
	if (errno != 0 || end == size || !end || *end != 'x')
		return -EINVAL;
	if (width <= 0 || width > INT_MAX)
		return -EINVAL;

	size = end + 1;
	errno = 0;
	height = strtol(size, &end, 10);
	if (errno != 0 || end == size || !end || *end != '\0')
		return -EINVAL;
	if (height <= 0 || height > INT_MAX)
		return -EINVAL;

	pixels = (long long)width * (long long)height;
	if (pixels < min_pixels || pixels > max_pixels)
		return -EINVAL;

	return 0;
}

static int64_t image_units_from_dims(int width, int height)
{
	int64_t pixels;

	if (width <= 0 || height <= 0)
		return 1;
	pixels = (int64_t)width * (int64_t)height;
	return (pixels + 999999) / 1000000;
}

static void image_report_usage(struct model *self, const struct image_payload *payload,
			       const struct image_result *result)
{
	struct model_usage usage;

	if (payload && payload->has_usage)
		usage = payload->usage;
	else
		memset(&usage, 0, sizeof(usage));
	snprintf(usage.provider, sizeof(usage.provider), "%s", self ? self->provider : "openai");
	snprintf(usage.model, sizeof(usage.model), "%s", self ? self->model_id : "");
	snprintf(usage.kind, sizeof(usage.kind), "model_image");
	if (!usage.usage_source[0])
		snprintf(usage.usage_source, sizeof(usage.usage_source), "estimated");
	usage.image_units = result ? image_units_from_dims(result->width, result->height) : 1;
	model_report_usage(self, &usage);
}

static const struct image_provider_ops *image_provider_resolve(const struct model *model)
{
	const char *name;

	if (!model)
		return NULL;
	name = model->adapter[0] ? model->adapter : model->provider;
	if (strcmp(name, "openai-images") == 0 ||
	    (!model->adapter[0] && strcmp(name, "openai") == 0))
		return image_openai_provider_borrow();
	if (strcmp(name, "volcengine-images") == 0 ||
	    (!model->adapter[0] && strcmp(name, "volcengine") == 0))
		return image_volcengine_provider_borrow();
	return NULL;
}

int image_gen_adapter_supported(const char *provider, const char *adapter)
{
	const char *name;

	if (!provider)
		return 0;
	name = adapter && adapter[0] ? adapter : provider;
	if (strcmp(name, "openai-images") == 0 || strcmp(name, "volcengine-images") == 0)
		return 1;
	if ((!adapter || !adapter[0]) &&
	    (strcmp(name, "openai") == 0 || strcmp(name, "volcengine") == 0))
		return 1;
	return 0;
}

const char *image_gen_adapter_name(const struct model *model)
{
	const struct image_provider_ops *ops = image_provider_resolve(model);

	return ops ? ops->name : NULL;
}

int image_gen_validate_size_for_model(const struct model *model, const char *size)
{
	const struct image_provider_ops *ops = image_provider_resolve(model);
	char normalized[64];

	if (!ops)
		return -ENOTSUP;
	return ops->normalize_size(model, size, normalized, sizeof(normalized));
}

void image_payload_cleanup(struct image_payload *payload)
{
	if (!payload)
		return;
	free(payload->data);
	memset(payload, 0, sizeof(*payload));
}

int image_provider_parse_response(struct model *model, const char *response_json,
				  struct image_payload *payload)
{
	cJSON *root MORPH_CLEANUP(morph_json_cleanup) = NULL;
	cJSON *data;
	cJSON *first;
	cJSON *value;
	cJSON *usage;

	if (!response_json || !payload)
		return -EINVAL;
	if (payload->data)
		return -EEXIST;
	struct image_payload parsed MORPH_CLEANUP(image_payload_cleanup) = {0};
	root = cJSON_Parse(response_json);
	if (!root)
		return MORPH_ERR_PARSE;
	data = cJSON_GetObjectItem(root, "data");
	if (!cJSON_IsArray(data) || cJSON_GetArraySize(data) == 0) {
		return MORPH_ERR_PROTOCOL;
	}
	first = cJSON_GetArrayItem(data, 0);
	value = cJSON_GetObjectItem(first, "b64_json");
	if (cJSON_IsString(value) && value->valuestring) {
		parsed.kind = IMAGE_PAYLOAD_BASE64;
		parsed.data = strdup(value->valuestring);
		if (!parsed.data) {
			return -ENOMEM;
		}
	} else {
		value = cJSON_GetObjectItem(first, "url");
		if (cJSON_IsString(value) && value->valuestring) {
			parsed.kind = IMAGE_PAYLOAD_URL;
			parsed.data = strdup(value->valuestring);
			if (!parsed.data) {
				return -ENOMEM;
			}
		}
	}
	if (!parsed.data) {
		return MORPH_ERR_PROTOCOL;
	}
	value = cJSON_GetObjectItem(root, "output_format");
	snprintf(parsed.output_format, sizeof(parsed.output_format), "%s",
		 cJSON_IsString(value) && value->valuestring ? value->valuestring : "png");
	value = cJSON_GetObjectItem(root, "created");
	if (cJSON_IsNumber(value))
		parsed.usage.created = (int64_t)value->valuedouble;
	usage = cJSON_GetObjectItem(root, "usage");
	if (cJSON_IsObject(usage)) {
		value = cJSON_GetObjectItem(usage, "input_tokens");
		if (cJSON_IsNumber(value))
			parsed.usage.input_tokens = (int64_t)value->valuedouble;
		value = cJSON_GetObjectItem(usage, "output_tokens");
		if (cJSON_IsNumber(value))
			parsed.usage.output_tokens = (int64_t)value->valuedouble;
		value = cJSON_GetObjectItem(usage, "total_tokens");
		if (cJSON_IsNumber(value))
			parsed.usage.total_tokens = (int64_t)value->valuedouble;
		snprintf(parsed.usage.usage_source, sizeof(parsed.usage.usage_source), "api");
		parsed.has_usage = 1;
	}

	(void)model;
	*payload = parsed;
	memset(&parsed, 0, sizeof(parsed));
	return 0;
}

static int image_materialize(const struct http_control *control,
			     const struct image_payload *payload, const char *output_dir,
			     struct image_result *result)
{
	unsigned char *decoded MORPH_CLEANUP(morph_ucharp_cleanup) = NULL;
	char *out_dir MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	char out_path[PATH_MAX];
	const char *ext;
	size_t decoded_len = 0;
	int rc;

	if (!payload || !payload->data || !result)
		return -EINVAL;
	out_dir =
		file_expand_path_copy(output_dir && output_dir[0] ? output_dir : "~/.morph/output");
	if (!out_dir)
		return -ENOMEM;
	rc = file_ensure_dir(out_dir);
	if (rc != 0)
		return rc;
	if (payload->kind == IMAGE_PAYLOAD_URL) {
		snprintf(result->url, sizeof(result->url), "%s", payload->data);
		rc = download_url(control, payload->data, out_dir, out_path, sizeof(out_path));
		if (rc != 0)
			return rc;
	} else {
		decoded = base64_decode_copy(payload->data, &decoded_len);
		if (!decoded || decoded_len == 0) {
			rc = MORPH_ERR_PARSE;
			return rc;
		}
		if (strcmp(payload->output_format, "jpeg") == 0 ||
		    strcmp(payload->output_format, "jpg") == 0) {
			ext = "jpg";
		} else if (strcmp(payload->output_format, "webp") == 0) {
			ext = "webp";
		} else if (strcmp(payload->output_format, "png") == 0) {
			ext = "png";
		} else {
			ext = image_gen_ext_from_magic(decoded, decoded_len);
		}
		rc = media_output_write(out_dir, "img", ext, decoded, decoded_len, out_path,
					sizeof(out_path));
		if (rc != 0)
			return rc;
	}
	snprintf(result->path, sizeof(result->path), "%s", out_path);
	{
		int channels = 0;
		unsigned char *image =
			stbi_load(out_path, &result->width, &result->height, &channels, 0);
		if (image)
			stbi_image_free(image);
	}
	return 0;
}

int image_gen_create(struct model *self, const char *prompt, const char *style, const char *size,
		     const char **image_paths, int image_count, const char *output_dir,
		     struct image_result *result)
{
	const struct image_provider_ops *ops;
	struct image_capabilities caps;
	struct image_payload payload MORPH_CLEANUP(image_payload_cleanup) = {0};
	struct image_request request;
	morph_buf_t prompt_buf MORPH_CLEANUP(morph_buf_cleanup) = {0};
	char normalized_size[64];
	int rc;

	if (!prompt || !result || image_count < 0 || image_count > IMAGE_GEN_MAX_REFERENCE_IMAGES ||
	    (image_count > 0 && !image_paths))
		return -EINVAL;
	for (int i = 0; i < image_count; i++) {
		if (!image_paths[i] || !image_paths[i][0])
			return -EINVAL;
	}
	memset(result, 0, sizeof(*result));
	memset(&payload, 0, sizeof(payload));
	if (!model_api_key_borrow(self)[0])
		return MORPH_ERR_NOT_CONFIGURED;
	ops = image_provider_resolve(self);
	if (!ops) {
		snprintf(self->last_error, sizeof(self->last_error),
			 "unsupported image adapter for provider '%s'", self->provider);
		return -ENOTSUP;
	}
	rc = ops->normalize_size(self, size, normalized_size, sizeof(normalized_size));
	if (rc != 0)
		return rc;
	rc = ops->capabilities(self, &caps);
	if (rc != 0)
		return rc;
	if (image_count > 1 && !caps.supports_multi_reference) {
		snprintf(self->last_error, sizeof(self->last_error),
			 "image adapter '%s' does not support multiple reference "
			 "images for model '%s'",
			 ops->name, self->model_id);
		return -ENOTSUP;
	}
	if (image_count > 0 && !caps.supports_edit) {
		snprintf(self->last_error, sizeof(self->last_error),
			 "image adapter '%s' does not support reference edits "
			 "for model '%s'",
			 ops->name, self->model_id);
		return -ENOTSUP;
	}
	if (morph_buf_init(&prompt_buf, strlen(prompt) + 64) != 0)
		return -ENOMEM;
	rc = morph_buf_puts(&prompt_buf, style_prefix(style));
	if (rc == 0)
		rc = morph_buf_puts(&prompt_buf, prompt);
	if (rc != 0) {
		morph_buf_cleanup(&prompt_buf);
		return rc;
	}
	memset(&request, 0, sizeof(request));
	request.prompt = morph_buf_cstr_borrow(&prompt_buf);
	request.size = normalized_size;
	request.reference_images = image_paths;
	request.reference_image_count = image_count;
	rc = ops->execute(self, &request, &payload);
	morph_buf_cleanup(&prompt_buf);
	if (rc != 0) {
		image_payload_cleanup(&payload);
		return rc;
	}
	rc = image_materialize(&self->http, &payload, output_dir, result);
	if (rc == 0)
		image_report_usage(self, &payload, result);
	image_payload_cleanup(&payload);
	return rc;
}
