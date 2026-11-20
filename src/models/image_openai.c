#include "image_provider.h"
#include "http/request.h"
#include <nb/buf.h>
#include <nb/cleanup.h>
#include "cJSON.h"
#include "errors.h"
#include <nb/file.h>
#include "cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int openai_is_gpt_image(const struct model *model)
{
	return model && strncmp(model->model_id, "gpt-image-", 10) == 0;
}

static int parse_dims(const char *size, long *width, long *height)
{
	char *end = NULL;

	if (!size || !width || !height)
		return NB_ERR_INVAL;
	errno = 0;
	*width = strtol(size, &end, 10);
	if (errno != 0 || end == size || !end || *end != 'x')
		return NB_ERR_INVAL;
	size = end + 1;
	errno = 0;
	*height = strtol(size, &end, 10);
	if (errno != 0 || end == size || !end || *end != '\0')
		return NB_ERR_INVAL;
	if (*width <= 0 || *height <= 0)
		return NB_ERR_INVAL;
	return 0;
}

static int openai_normalize_gpt_size(const char *size, char *out,
				      size_t out_cap)
{
	long width;
	long height;
	long long pixels;
	long long short_edge;
	long long long_edge;

	if (!size || !*size || strcmp(size, "auto") == 0) {
		snprintf(out, out_cap, "auto");
		return 0;
	}
	if (strcmp(size, "2k") == 0) {
		snprintf(out, out_cap, "2048x2048");
		return 0;
	}
	if (strcmp(size, "4k") == 0) {
		snprintf(out, out_cap, "3840x2160");
		return 0;
	}
	if (parse_dims(size, &width, &height) != 0)
		return NB_ERR_INVAL;
	pixels = (long long)width * (long long)height;
	short_edge = width < height ? width : height;
	long_edge = width > height ? width : height;
	if (width > 3840 || height > 3840 ||
	    width % 16 != 0 || height % 16 != 0 ||
	    pixels < 655360LL || pixels > 8294400LL ||
	    long_edge > short_edge * 3)
		return NB_ERR_INVAL;
	snprintf(out, out_cap, "%ldx%ld", width, height);
	return 0;
}

static int openai_normalize_dalle_size(const struct model *model,
					const char *size, char *out,
					size_t out_cap)
{
	const char *normalized = size;

	if (!size || !*size || strcmp(size, "auto") == 0 ||
	    strcmp(size, "2k") == 0 || strcmp(size, "4k") == 0)
		normalized = "1024x1024";
	if (model && strcmp(model->model_id, "dall-e-2") == 0) {
		if (strcmp(normalized, "256x256") != 0 &&
		    strcmp(normalized, "512x512") != 0 &&
		    strcmp(normalized, "1024x1024") != 0)
			return NB_ERR_INVAL;
	} else if (strcmp(normalized, "1024x1024") != 0 &&
		   strcmp(normalized, "1024x1792") != 0 &&
		   strcmp(normalized, "1792x1024") != 0) {
		return NB_ERR_INVAL;
	}
	snprintf(out, out_cap, "%s", normalized);
	return 0;
}

static int openai_normalize_size(const struct model *model, const char *size,
				 char *out, size_t out_cap)
{
	if (!out || out_cap == 0)
		return NB_ERR_INVAL;
	if (openai_is_gpt_image(model))
		return openai_normalize_gpt_size(size, out, out_cap);
	return openai_normalize_dalle_size(model, size, out, out_cap);
}

static int openai_capabilities(const struct model *model,
			       struct image_capabilities *caps)
{
	if (!caps)
		return NB_ERR_INVAL;
	memset(caps, 0, sizeof(*caps));
	caps->supports_generate = 1;
	if (openai_is_gpt_image(model)) {
		caps->supports_edit = 1;
		caps->supports_mask = 1;
		caps->supports_multi_reference = 1;
	}
	return 0;
}

static int openai_post_json(struct model *model,
			    const struct image_request *request,
			    struct image_payload *payload)
{
	cJSON *body = NULL;
	char *body_text NB_CLEANUP(cJSON_FreeTextOwner) = NULL;
	int rc = 0;

	body = cJSON_CreateObject();
	if (!body)
		return NB_ERR_NOMEM;
	cJSON_AddStringToObject(body, "model", model->model_id);
	cJSON_AddStringToObject(body, "prompt", request->prompt);
	cJSON_AddNumberToObject(body, "n", 1);
	cJSON_AddStringToObject(body, "size", request->size);
	if (!openai_is_gpt_image(model))
		cJSON_AddStringToObject(body, "response_format", "url");
	body_text = cJSON_PrintUnformatted(body);
	cJSON_Delete(body);
	if (!body_text)
		return NB_ERR_NOMEM;

	rc = image_provider_post_json(model, "/images/generations", "OpenAI image API", body_text,
				      payload);

	return rc;
}

static const char *image_content_type(const char *path)
{
	const char *ext = path ? strrchr(path, '.') : NULL;

	if (ext && (strcasecmp(ext, ".jpg") == 0 ||
		    strcasecmp(ext, ".jpeg") == 0))
		return "image/jpeg";
	if (ext && strcasecmp(ext, ".webp") == 0)
		return "image/webp";
	return "image/png";
}

static int openai_post_edit(struct model *model,
			    const struct image_request *request,
			    struct image_payload *payload)
{
	nb_vec part_storage NB_CLEANUP(nb_vec_cleanup) = {0};
	nb_http_part *parts;
	nb_buf resp_body NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf auth NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf url NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *headers[1];
	int part_count;
	int idx = 0;
	int rc = 0;

	part_count = 3 + request->reference_image_count +
		(request->mask_path ? 1 : 0);
	nb_vec_init(&part_storage, sizeof(*parts));
	parts = nb_vec_push_n(&part_storage, (size_t)part_count);
	if (!parts)
		return NB_ERR_NOMEM;
	parts[idx++] = (nb_http_part){.name = "model", .data = nb_str_from_cstr(model->model_id)};
	parts[idx++] = (nb_http_part){.name = "prompt", .data = nb_str_from_cstr(request->prompt)};
	parts[idx++] = (nb_http_part){.name = "size", .data = nb_str_from_cstr(request->size)};
	for (int i = 0; i < request->reference_image_count; i++) {
		parts[idx++] = (nb_http_part){
			.name = "image[]",
			.file_path = request->reference_images[i],
			.content_type = image_content_type(request->reference_images[i])};
	}
	if (request->mask_path) {
		parts[idx++] =
			(nb_http_part){.name = "mask",
				       .file_path = request->mask_path,
				       .content_type = image_content_type(request->mask_path)};
	}
	rc = image_provider_build_request_meta(model, "/images/edits", &url, &auth);
	if (rc != 0) {
		return rc;
	}
	headers[0] = nb_buf_cstr(&auth);
	nb_http_client *http_client NB_CLEANUP(http_client_cleanup) = NULL;
	nb_http_call *http_call NB_CLEANUP(http_call_cleanup) = NULL;
	nb_vec native_headers NB_CLEANUP(nb_vec_cleanup) = {0};
	nb_http_request http_request;

	nb_vec_init(&native_headers, sizeof(nb_http_header));
	rc = http_headers_parse(&native_headers, headers, 1);
	http_request_init(&http_request, "POST", nb_buf_cstr(&url));
	http_request.body = NB_STR_NULL;
	http_request.headers =
		(nb_http_headers){(const nb_http_header *)native_headers.data, native_headers.len};
	http_request.timeouts.total_ms = http_timeout_ms(model->timeout_seconds);
	http_request.parts = parts;
	http_request.part_count = (size_t)idx;
	if (rc == NB_OK)
		rc = http_client_open(&model->http, &http_client);

	if (rc == NB_OK)
		rc = http_request_run(&model->http, http_client, &http_request, NULL, &http_call);

	const nb_http_result *http_result = nb_http_call_result(http_call);
	const nb_http_response *resp = http_result ? http_result->response : NULL;
	if (rc == NB_OK)
		rc = nb_http_call_take_body(http_call, &resp_body, NULL);

	nb_buf_cleanup(&url);
	nb_buf_cleanup(&auth);
	if (rc != 0) {
		return rc;
	}
	if (resp->status_code != 200) {
		snprintf(model->last_error, sizeof(model->last_error),
			 "OpenAI image edit API returned HTTP %d: %.400s", resp->status_code,
			 resp_body.ptr ? resp_body.ptr : "");
		return MORPH_ERR_API;
	}
	rc = image_provider_parse_response(model, nb_buf_cstr(&resp_body), payload);
	return rc;
}

static int openai_execute(struct model *model,
			  const struct image_request *request,
			  struct image_payload *payload)
{
	if (!model || !request || !request->prompt || !payload)
		return NB_ERR_INVAL;
	if (payload->data)
		return NB_ERR_EXISTS;
	if (request->reference_image_count > 0) {
		if (!openai_is_gpt_image(model))
			return NB_ERR_UNSUPPORTED;
		return openai_post_edit(model, request, payload);
	}
	return openai_post_json(model, request, payload);
}

const struct image_provider_ops *image_openai_provider_borrow(void)
{
	static const struct image_provider_ops ops = {
		.name = "openai-images",
		.capabilities = openai_capabilities,
		.normalize_size = openai_normalize_size,
		.execute = openai_execute,
	};

	return &ops;
}
