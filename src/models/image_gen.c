#define _GNU_SOURCE
#include "image_gen.h"
#include "image_provider.h"
#include "models/llm.h"
#include "util/log.h"
#include "util/file.h"
#include "util/base64.h"
#include "util/image_util.h"
#include "util/arena.h"
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
		MORPH_RETURN(-EINVAL);
	if (width <= 0 || width > INT_MAX)
		MORPH_RETURN(-EINVAL);

	size = end + 1;
	errno = 0;
	height = strtol(size, &end, 10);
	if (errno != 0 || end == size || !end || *end != '\0')
		MORPH_RETURN(-EINVAL);
	if (height <= 0 || height > INT_MAX)
		MORPH_RETURN(-EINVAL);

	pixels = (long long)width * (long long)height;
	if (pixels < min_pixels || pixels > max_pixels)
		MORPH_RETURN(-EINVAL);

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

static void image_report_usage(struct model *self,
			       const struct image_payload *payload,
			       const struct image_result *result)
{
	struct model_usage usage;

	if (payload && payload->has_usage)
		usage = payload->usage;
	else
		memset(&usage, 0, sizeof(usage));
	snprintf(usage.provider, sizeof(usage.provider), "%s",
		 self ? self->provider : "openai");
	snprintf(usage.model, sizeof(usage.model), "%s",
		 self ? self->model_id : "");
	snprintf(usage.kind, sizeof(usage.kind), "model_image");
	if (!usage.usage_source[0])
		snprintf(usage.usage_source, sizeof(usage.usage_source),
			 "estimated");
	usage.image_units = result ?
		image_units_from_dims(result->width, result->height) : 1;
	model_report_usage(&usage);
}

static const struct image_provider_ops *image_provider_resolve(
	const struct model *model)
{
	const char *name;

	if (!model)
		return NULL;
	name = model->adapter[0] ? model->adapter : model->provider;
	if (strcmp(name, "openai-images") == 0 ||
	    (!model->adapter[0] && strcmp(name, "openai") == 0))
		return image_openai_provider();
	if (strcmp(name, "volcengine-images") == 0 ||
	    (!model->adapter[0] && strcmp(name, "volcengine") == 0))
		return image_volcengine_provider();
	return NULL;
}

int image_gen_adapter_supported(const char *provider, const char *adapter)
{
	const char *name;

	if (!provider)
		return 0;
	name = adapter && adapter[0] ? adapter : provider;
	if (strcmp(name, "openai-images") == 0 ||
	    strcmp(name, "volcengine-images") == 0)
		return 1;
	if ((!adapter || !adapter[0]) &&
	    (strcmp(name, "openai") == 0 ||
	     strcmp(name, "volcengine") == 0))
		return 1;
	return 0;
}

const char *image_gen_adapter_name(const struct model *model)
{
	const struct image_provider_ops *ops = image_provider_resolve(model);

	return ops ? ops->name : NULL;
}

int image_gen_validate_size_for_model(const struct model *model,
				      const char *size)
{
	const struct image_provider_ops *ops = image_provider_resolve(model);
	char normalized[64];

	if (!ops)
		MORPH_RETURN(-ENOTSUP);
	return ops->normalize_size(model, size, normalized,
				   sizeof(normalized));
}

void image_payload_cleanup(struct image_payload *payload)
{
	if (!payload)
		return;
	free(payload->data);
	memset(payload, 0, sizeof(*payload));
}

int image_provider_parse_response(struct model *model,
				  const char *response_json,
				  struct image_payload *payload)
{
	cJSON *root = NULL;
	cJSON *data;
	cJSON *first;
	cJSON *value;
	cJSON *usage;

	if (!response_json || !payload)
		MORPH_RETURN(-EINVAL);
	memset(payload, 0, sizeof(*payload));
	root = cJSON_Parse(response_json);
	if (!root)
		MORPH_RETURN(MORPH_ERR_PARSE);
	data = cJSON_GetObjectItem(root, "data");
	if (!cJSON_IsArray(data) || cJSON_GetArraySize(data) == 0) {
		cJSON_Delete(root);
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	}
	first = cJSON_GetArrayItem(data, 0);
	value = cJSON_GetObjectItem(first, "b64_json");
	if (cJSON_IsString(value) && value->valuestring) {
		payload->kind = IMAGE_PAYLOAD_BASE64;
		payload->data = strdup(value->valuestring);
		if (!payload->data) {
			cJSON_Delete(root);
			MORPH_RETURN(-ENOMEM);
		}
	} else {
		value = cJSON_GetObjectItem(first, "url");
		if (cJSON_IsString(value) && value->valuestring) {
			payload->kind = IMAGE_PAYLOAD_URL;
			payload->data = strdup(value->valuestring);
			if (!payload->data) {
				cJSON_Delete(root);
				MORPH_RETURN(-ENOMEM);
			}
		}
	}
	if (!payload->data) {
		cJSON_Delete(root);
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	}
	value = cJSON_GetObjectItem(root, "output_format");
	snprintf(payload->output_format, sizeof(payload->output_format), "%s",
		 cJSON_IsString(value) && value->valuestring
			 ? value->valuestring : "png");
	value = cJSON_GetObjectItem(root, "created");
	if (cJSON_IsNumber(value))
		payload->usage.created = (int64_t)value->valuedouble;
	usage = cJSON_GetObjectItem(root, "usage");
	if (cJSON_IsObject(usage)) {
		value = cJSON_GetObjectItem(usage, "input_tokens");
		if (cJSON_IsNumber(value))
			payload->usage.input_tokens =
				(int64_t)value->valuedouble;
		value = cJSON_GetObjectItem(usage, "output_tokens");
		if (cJSON_IsNumber(value))
			payload->usage.output_tokens =
				(int64_t)value->valuedouble;
		value = cJSON_GetObjectItem(usage, "total_tokens");
		if (cJSON_IsNumber(value))
			payload->usage.total_tokens =
				(int64_t)value->valuedouble;
		snprintf(payload->usage.usage_source,
			 sizeof(payload->usage.usage_source), "api");
		payload->has_usage = 1;
	}
	cJSON_Delete(root);
	(void)model;
	return 0;
}

static int image_materialize(const struct image_payload *payload,
			     const char *output_dir,
			     struct image_result *result)
{
	unsigned char *decoded = NULL;
	char *out_dir = NULL;
	char out_path[PATH_MAX];
	const char *ext;
	size_t decoded_len = 0;
	int rc;

	if (!payload || !payload->data || !result)
		MORPH_RETURN(-EINVAL);
	out_dir = file_expand_path(output_dir && output_dir[0]
				   ? output_dir : "~/.morph/output");
	if (!out_dir)
		MORPH_RETURN(-ENOMEM);
	rc = file_ensure_dir(out_dir);
	if (rc != 0)
		goto out;
	if (payload->kind == IMAGE_PAYLOAD_URL) {
		snprintf(result->url, sizeof(result->url), "%s",
			 payload->data);
		rc = download_url(payload->data, out_dir, out_path,
				  sizeof(out_path));
		if (rc != 0)
			goto out;
	} else {
		decoded = base64_decode(payload->data, &decoded_len);
		if (!decoded || decoded_len == 0) {
			rc = MORPH_ERR_PARSE;
			goto out;
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
		snprintf(out_path, sizeof(out_path), "%s/img_%lld.%s",
			 out_dir, (long long)time(NULL), ext);
		rc = file_write_all(out_path, (const char *)decoded,
				    decoded_len);
		if (rc != 0)
			goto out;
	}
	snprintf(result->path, sizeof(result->path), "%s", out_path);
	{
		int channels = 0;
		unsigned char *image = stbi_load(
			out_path, &result->width, &result->height,
			&channels, 0);
		if (image)
			stbi_image_free(image);
	}
	rc = 0;

out:
	free(decoded);
	free(out_dir);
	return rc;
}

int image_gen_create(struct model *self, const char *prompt, const char *style,
		     const char *size, const char **image_paths,
		     int image_count,
		     const char *output_dir,
		     struct image_result *result)
{
	const struct image_provider_ops *ops;
	struct image_capabilities caps;
	struct image_payload payload;
	struct image_request request;
	morph_buf_t prompt_buf;
	char normalized_size[64];
	int rc;

	if (!prompt || !result || image_count < 0 ||
	    image_count > IMAGE_GEN_MAX_REFERENCE_IMAGES ||
	    (image_count > 0 && !image_paths))
		MORPH_RETURN(-EINVAL);
	for (int i = 0; i < image_count; i++) {
		if (!image_paths[i] || !image_paths[i][0])
			MORPH_RETURN(-EINVAL);
	}
	memset(result, 0, sizeof(*result));
	memset(&payload, 0, sizeof(payload));
	if (!self || !self->api_key[0])
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	ops = image_provider_resolve(self);
	if (!ops) {
		snprintf(self->last_error, sizeof(self->last_error),
			 "unsupported image adapter for provider '%s'",
			 self->provider);
		MORPH_RETURN(-ENOTSUP);
	}
	rc = ops->normalize_size(self, size, normalized_size,
				 sizeof(normalized_size));
	if (rc != 0)
		return rc;
	rc = ops->capabilities(self, &caps);
	if (rc != 0)
		return rc;
	if (image_count > 1 && !caps.supports_multi_reference) {
		snprintf(self->last_error, sizeof(self->last_error),
			 "image adapter '%s' does not support multiple reference "
			 "images for model '%s'", ops->name, self->model_id);
		MORPH_RETURN(-ENOTSUP);
	}
	if (image_count > 0 && !caps.supports_edit) {
		snprintf(self->last_error, sizeof(self->last_error),
			 "image adapter '%s' does not support reference edits "
			 "for model '%s'", ops->name, self->model_id);
		MORPH_RETURN(-ENOTSUP);
	}
	if (morph_buf_init(&prompt_buf, strlen(prompt) + 64) != 0)
		MORPH_RETURN(-ENOMEM);
	rc = morph_buf_puts(&prompt_buf, style_prefix(style));
	if (rc == 0)
		rc = morph_buf_puts(&prompt_buf, prompt);
	if (rc != 0) {
		morph_buf_cleanup(&prompt_buf);
		return rc;
	}
	memset(&request, 0, sizeof(request));
	request.prompt = morph_buf_cstr(&prompt_buf);
	request.size = normalized_size;
	request.reference_images = image_paths;
	request.reference_image_count = image_count;
	rc = ops->execute(self, &request, &payload);
	morph_buf_cleanup(&prompt_buf);
	if (rc != 0) {
		image_payload_cleanup(&payload);
		return rc;
	}
	rc = image_materialize(&payload, output_dir, result);
	if (rc == 0)
		image_report_usage(self, &payload, result);
	image_payload_cleanup(&payload);
	return rc;
}
