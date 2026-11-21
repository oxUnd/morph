#define _GNU_SOURCE
#include "image_gen.h"
#include "image_provider.h"
#include "media_output.h"
#include "models/llm.h"
#include <nb/log.h>
#include <nb/file.h>
#include <nb/encoding.h>
#include "media/image.h"
#include <nb/cleanup.h>
#include "cJSON.h"
#include <nb/buf.h>
#include "errors.h"
#include "http/request.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>

#include "stb_image.h"

int image_provider_build_request_meta(const struct model *model, const char *path, nb_buf *url,
				      nb_buf *auth)
{
	if (!model || !path || !url || !auth || url == auth)
		return NB_ERR_INVAL;
	if (url->ptr || auth->ptr)
		return NB_ERR_EXISTS;
	nb_buf url_owner NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf auth_owner NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&url_owner),
		  nb_buf_reserve(&url_owner, strlen(model->api_base) + strlen(path) + 1)
			  ? 0
			  : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_printf(&url_owner, "%s%s", model->api_base, path) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = model_authorization_header_create(model, &auth_owner);
	if (rc != 0)
		return rc;
	(nb_buf_destroy(url), nb_buf_move(url, &url_owner));
	(nb_buf_destroy(auth), nb_buf_move(auth, &auth_owner));
	return 0;
}

int image_provider_post_json(struct model *model, const char *path, const char *label,
			     const char *body, struct image_payload *payload)
{
	nb_buf response_body NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf url NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf auth NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *headers[1];
	int rc;

	if (!label || !body || !payload)
		return NB_ERR_INVAL;
	if (payload->data)
		return NB_ERR_EXISTS;
	rc = image_provider_build_request_meta(model, path, &url, &auth);
	if (rc != 0)
		return rc;
	headers[0] = nb_buf_cstr(&auth);
	nb_http_client *http_client NB_CLEANUP(http_client_cleanup) = NULL;
	nb_http_call *http_call NB_CLEANUP(http_call_cleanup) = NULL;
	nb_vec native_headers NB_CLEANUP(nb_vec_cleanup) = {0};
	nb_http_request http_request;

	nb_vec_init(&native_headers, sizeof(nb_http_header));
	rc = http_headers_parse(&native_headers, headers, 1);
	if (rc == NB_OK) {
		nb_http_header content_type = {NB_STR_LIT("Content-Type"),
					       NB_STR_LIT("application/json")};
		if (!nb_vec_push_copy(&native_headers, &content_type))
			rc = NB_ERR_NOMEM;
	}

	http_request_init(&http_request, "POST", nb_buf_cstr(&url));
	http_request.body = nb_str_from_cstr(body);
	http_request.headers =
		(nb_http_headers){(const nb_http_header *)native_headers.data, native_headers.len};
	http_request.timeouts.total_ms = http_timeout_ms(model->timeout_seconds);
	if (rc == NB_OK)
		rc = http_client_open(&model->http, &http_client);

	if (rc == NB_OK)
		rc = http_request_run(&model->http, http_client, &http_request, NULL, &http_call);

	const nb_http_result *http_result = nb_http_call_result(http_call);
	const nb_http_response *response = http_result ? http_result->response : NULL;
	if (rc == NB_OK)
		rc = nb_http_call_take_body(http_call, &response_body, NULL);

	nb_buf_cleanup(&auth);
	nb_buf_cleanup(&url);
	if (rc == 0) {
		if (response->status_code != 200) {
			snprintf(model->last_error, sizeof(model->last_error),
				 "%s returned HTTP %d: %.400s", label, response->status_code,
				 response_body.ptr ? response_body.ptr : "");
			rc = MORPH_ERR_API;
		} else {
			rc = image_provider_parse_response(model, nb_buf_cstr(&response_body),
							   payload);
		}
	}
	return rc;
}

const char *image_gen_ext_from_headers(nb_http_headers headers)
{
	nb_str type = NB_STR_NULL;
	if (!nb_http_headers_get(headers, NB_STR_LIT("Content-Type"), &type))
		return NULL;

	return image_format_extension(image_format_from_mime(type));
}

const char *image_gen_ext_from_magic(const unsigned char *data, size_t len)
{
	const char *extension = image_format_extension(image_format_detect(data, len));
	return extension ? extension : "png";
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
		return NB_ERR_INVAL;
	if (width <= 0 || width > INT_MAX)
		return NB_ERR_INVAL;

	size = end + 1;
	errno = 0;
	height = strtol(size, &end, 10);
	if (errno != 0 || end == size || !end || *end != '\0')
		return NB_ERR_INVAL;
	if (height <= 0 || height > INT_MAX)
		return NB_ERR_INVAL;

	pixels = (long long)width * (long long)height;
	if (pixels < min_pixels || pixels > max_pixels)
		return NB_ERR_INVAL;

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
		return NB_ERR_UNSUPPORTED;
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
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	cJSON *data;
	cJSON *first;
	cJSON *value;
	cJSON *usage;

	if (!response_json || !payload)
		return NB_ERR_INVAL;
	if (payload->data)
		return NB_ERR_EXISTS;
	struct image_payload parsed NB_CLEANUP(image_payload_cleanup) = {0};
	root = cJSON_Parse(response_json);
	if (!root)
		return NB_ERR_PARSE;
	data = cJSON_GetObjectItem(root, "data");
	if (!cJSON_IsArray(data) || cJSON_GetArraySize(data) == 0) {
		return NB_ERR_PROTOCOL;
	}
	first = cJSON_GetArrayItem(data, 0);
	value = cJSON_GetObjectItem(first, "b64_json");
	if (cJSON_IsString(value) && value->valuestring) {
		parsed.kind = IMAGE_PAYLOAD_BASE64;
		parsed.data = strdup(value->valuestring);
		if (!parsed.data) {
			return NB_ERR_NOMEM;
		}
	} else {
		value = cJSON_GetObjectItem(first, "url");
		if (cJSON_IsString(value) && value->valuestring) {
			parsed.kind = IMAGE_PAYLOAD_URL;
			parsed.data = strdup(value->valuestring);
			if (!parsed.data) {
				return NB_ERR_NOMEM;
			}
		}
	}
	if (!parsed.data) {
		return NB_ERR_PROTOCOL;
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
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int expand_status_1 = NB_OK;

	nb_buf decoded NB_CLEANUP(nb_buf_destroy) = {0};
	char *out_dir NB_CLEANUP(nb_charp_cleanup) = NULL;
	char out_path[PATH_MAX];
	const char *ext;
	size_t decoded_len = 0;
	int rc;

	if (!payload || !payload->data || !result)
		return NB_ERR_INVAL;
	out_dir = ((expand_status_1 = nb_path_expand(&file_value_1, output_dir &&
		output_dir[0] ? output_dir : "~/.morph/output", NULL)) == NB_OK
			   ? strdup(nb_buf_cstr(&file_value_1))
			   : NULL);
	if (!out_dir)
		return expand_status_1 != NB_OK ? expand_status_1 : NB_ERR_NOMEM;
	rc = nb_file_mkdir(out_dir, true, NULL);
	if (rc != 0)
		return rc;
	if (payload->kind == IMAGE_PAYLOAD_URL) {
		snprintf(result->url, sizeof(result->url), "%s", payload->data);
		rc = media_output_download(control, payload->data, out_dir, "img", NULL,
			out_path, sizeof(out_path));
		if (rc != 0)
			return rc;
	} else {
		nb_status status = nb_base64_decode(nb_str_from_cstr(payload->data), NB_B64_STD,
						    &decoded, NULL);
		decoded_len = nb_buf_len(&decoded);
		if (status != NB_OK || decoded_len == 0) {
			rc = NB_ERR_PARSE;
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
			ext = image_gen_ext_from_magic((const unsigned char *)nb_buf_cstr(&decoded),
						       decoded_len);
		}
		rc = media_output_write(out_dir, "img", ext, nb_buf_cstr(&decoded), decoded_len,
					out_path, sizeof(out_path));
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
	struct image_payload payload NB_CLEANUP(image_payload_cleanup) = {0};
	struct image_request request;
	nb_buf prompt_buf NB_CLEANUP(nb_buf_cleanup) = {0};
	char normalized_size[64];
	int rc;

	if (!prompt || !result || image_count < 0 || image_count > IMAGE_GEN_MAX_REFERENCE_IMAGES ||
	    (image_count > 0 && !image_paths))
		return NB_ERR_INVAL;
	for (int i = 0; i < image_count; i++) {
		if (!image_paths[i] || !image_paths[i][0])
			return NB_ERR_INVAL;
	}
	memset(result, 0, sizeof(*result));
	memset(&payload, 0, sizeof(payload));
	if (!model_api_key_borrow(self)[0])
		return MORPH_ERR_NOT_CONFIGURED;
	ops = image_provider_resolve(self);
	if (!ops) {
		snprintf(self->last_error, sizeof(self->last_error),
			 "unsupported image adapter for provider '%s'", self->provider);
		return NB_ERR_UNSUPPORTED;
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
		return NB_ERR_UNSUPPORTED;
	}
	if (image_count > 0 && !caps.supports_edit) {
		snprintf(self->last_error, sizeof(self->last_error),
			 "image adapter '%s' does not support reference edits "
			 "for model '%s'",
			 ops->name, self->model_id);
		return NB_ERR_UNSUPPORTED;
	}
	if ((nb_buf_init(&prompt_buf),
	     nb_buf_reserve(&prompt_buf, strlen(prompt) + 64) ? 0 : NB_ERR_NOMEM) != 0)
		return NB_ERR_NOMEM;
	rc = (nb_buf_append_cstr(&prompt_buf, style_prefix(style)) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_append_cstr(&prompt_buf, prompt) ? 0 : NB_ERR_NOMEM);
	if (rc != 0) {
		nb_buf_cleanup(&prompt_buf);
		return rc;
	}
	memset(&request, 0, sizeof(request));
	request.prompt = nb_buf_cstr(&prompt_buf);
	request.size = normalized_size;
	request.reference_images = image_paths;
	request.reference_image_count = image_count;
	rc = ops->execute(self, &request, &payload);
	nb_buf_cleanup(&prompt_buf);
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
