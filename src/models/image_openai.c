#include "image_provider.h"
#include "http/client.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/file.h"
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
		MORPH_RETURN(-EINVAL);
	errno = 0;
	*width = strtol(size, &end, 10);
	if (errno != 0 || end == size || !end || *end != 'x')
		MORPH_RETURN(-EINVAL);
	size = end + 1;
	errno = 0;
	*height = strtol(size, &end, 10);
	if (errno != 0 || end == size || !end || *end != '\0')
		MORPH_RETURN(-EINVAL);
	if (*width <= 0 || *height <= 0)
		MORPH_RETURN(-EINVAL);
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
		MORPH_RETURN(-EINVAL);
	pixels = (long long)width * (long long)height;
	short_edge = width < height ? width : height;
	long_edge = width > height ? width : height;
	if (width > 3840 || height > 3840 ||
	    width % 16 != 0 || height % 16 != 0 ||
	    pixels < 655360LL || pixels > 8294400LL ||
	    long_edge > short_edge * 3)
		MORPH_RETURN(-EINVAL);
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
			MORPH_RETURN(-EINVAL);
	} else if (strcmp(normalized, "1024x1024") != 0 &&
		   strcmp(normalized, "1024x1792") != 0 &&
		   strcmp(normalized, "1792x1024") != 0) {
		MORPH_RETURN(-EINVAL);
	}
	snprintf(out, out_cap, "%s", normalized);
	return 0;
}

static int openai_normalize_size(const struct model *model, const char *size,
				 char *out, size_t out_cap)
{
	if (!out || out_cap == 0)
		MORPH_RETURN(-EINVAL);
	if (openai_is_gpt_image(model))
		return openai_normalize_gpt_size(size, out, out_cap);
	return openai_normalize_dalle_size(model, size, out, out_cap);
}

static int openai_capabilities(const struct model *model,
			       struct image_capabilities *caps)
{
	if (!caps)
		MORPH_RETURN(-EINVAL);
	memset(caps, 0, sizeof(*caps));
	caps->supports_generate = 1;
	if (openai_is_gpt_image(model)) {
		caps->supports_edit = 1;
		caps->supports_mask = 1;
		caps->supports_multi_reference = 1;
	}
	return 0;
}

static int openai_build_request_meta(const struct model *model,
				     const char *path,
				     morph_buf_t *url,
				     morph_buf_t *auth)
{
	int rc;

	if (!model || !path || !url || !auth)
		MORPH_RETURN(-EINVAL);
	rc = morph_buf_init(url, strlen(model->api_base) + strlen(path) + 1);
	if (rc != 0)
		return rc;
	rc = morph_buf_init(auth, strlen(model->api_key) + 24);
	if (rc != 0)
		goto out;
	rc = morph_buf_printf(url, "%s%s", model->api_base, path);
	if (rc == 0)
		rc = morph_buf_printf(auth, "Authorization: Bearer %s",
				      model->api_key);

out:
	if (rc != 0) {
		morph_buf_cleanup(url);
		morph_buf_cleanup(auth);
	}
	return rc;
}

static int openai_post_json(struct model *model,
			    const struct image_request *request,
			    struct image_payload *payload)
{
	struct http_response resp = {0};
	morph_buf_t auth = {0};
	morph_buf_t url = {0};
	cJSON *body = NULL;
	char *body_text = NULL;
	const char *headers[1];
	int rc = 0;

	body = cJSON_CreateObject();
	if (!body)
		MORPH_RETURN(-ENOMEM);
	cJSON_AddStringToObject(body, "model", model->model_id);
	cJSON_AddStringToObject(body, "prompt", request->prompt);
	cJSON_AddNumberToObject(body, "n", 1);
	cJSON_AddStringToObject(body, "size", request->size);
	if (!openai_is_gpt_image(model))
		cJSON_AddStringToObject(body, "response_format", "url");
	body_text = cJSON_PrintUnformatted(body);
	cJSON_Delete(body);
	if (!body_text)
		MORPH_RETURN(-ENOMEM);

	rc = openai_build_request_meta(model, "/images/generations",
				       &url, &auth);
	if (rc != 0) {
		free(body_text);
		return rc;
	}
	headers[0] = morph_buf_cstr(&auth);
	rc = http_post_ex_timeout(morph_buf_cstr(&url), body_text,
				  strlen(body_text),
				  "application/json", headers, 1,
				  model->timeout_seconds, &resp);
	free(body_text);
	morph_buf_cleanup(&url);
	morph_buf_cleanup(&auth);
	if (rc != 0) {
		http_response_free(&resp);
		return rc;
	}
	if (resp.status_code != 200) {
		snprintf(model->last_error, sizeof(model->last_error),
			 "OpenAI image API returned HTTP %d: %.400s",
			 resp.status_code,
			 resp.body.data ? resp.body.data : "");
		http_response_free(&resp);
		MORPH_RETURN(MORPH_ERR_API);
	}
	rc = image_provider_parse_response(model, resp.body.data, payload);
	http_response_free(&resp);
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
	struct http_multipart_part *parts = NULL;
	struct http_response resp = {0};
	morph_buf_t auth = {0};
	morph_buf_t url = {0};
	const char *headers[1];
	int part_count;
	int idx = 0;
	int rc = 0;

	part_count = 3 + request->reference_image_count +
		(request->mask_path ? 1 : 0);
	parts = calloc((size_t)part_count, sizeof(*parts));
	if (!parts)
		MORPH_RETURN(-ENOMEM);
	parts[idx++] = (struct http_multipart_part){
		HTTP_MULTIPART_TEXT, "model", model->model_id, NULL
	};
	parts[idx++] = (struct http_multipart_part){
		HTTP_MULTIPART_TEXT, "prompt", request->prompt, NULL
	};
	parts[idx++] = (struct http_multipart_part){
		HTTP_MULTIPART_TEXT, "size", request->size, NULL
	};
	for (int i = 0; i < request->reference_image_count; i++) {
		parts[idx++] = (struct http_multipart_part){
			HTTP_MULTIPART_FILE, "image[]",
			request->reference_images[i],
			image_content_type(request->reference_images[i])
		};
	}
	if (request->mask_path) {
		parts[idx++] = (struct http_multipart_part){
			HTTP_MULTIPART_FILE, "mask", request->mask_path,
			image_content_type(request->mask_path)
		};
	}
	rc = openai_build_request_meta(model, "/images/edits", &url,
				       &auth);
	if (rc != 0) {
		free(parts);
		return rc;
	}
	headers[0] = morph_buf_cstr(&auth);
	rc = http_post_multipart_ex(morph_buf_cstr(&url), parts, idx,
				    headers, 1,
				    model->timeout_seconds, &resp);
	free(parts);
	morph_buf_cleanup(&url);
	morph_buf_cleanup(&auth);
	if (rc != 0) {
		http_response_free(&resp);
		return rc;
	}
	if (resp.status_code != 200) {
		snprintf(model->last_error, sizeof(model->last_error),
			 "OpenAI image edit API returned HTTP %d: %.400s",
			 resp.status_code,
			 resp.body.data ? resp.body.data : "");
		http_response_free(&resp);
		MORPH_RETURN(MORPH_ERR_API);
	}
	rc = image_provider_parse_response(model, resp.body.data, payload);
	http_response_free(&resp);
	return rc;
}

static int openai_execute(struct model *model,
			  const struct image_request *request,
			  struct image_payload *payload)
{
	if (!model || !request || !request->prompt || !payload)
		MORPH_RETURN(-EINVAL);
	if (request->reference_image_count > 0) {
		if (!openai_is_gpt_image(model))
			MORPH_RETURN(-ENOTSUP);
		return openai_post_edit(model, request, payload);
	}
	return openai_post_json(model, request, payload);
}

const struct image_provider_ops *image_openai_provider(void)
{
	static const struct image_provider_ops ops = {
		.name = "openai-images",
		.capabilities = openai_capabilities,
		.normalize_size = openai_normalize_size,
		.execute = openai_execute,
	};

	return &ops;
}
