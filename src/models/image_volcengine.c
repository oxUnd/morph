#include "image_provider.h"
#include "image_gen.h"
#include "http/client.h"
#include "util/arena.h"
#include "util/base64.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/image_util.h"
#include "cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int volcengine_capabilities(const struct model *model,
				   struct image_capabilities *caps)
{
	(void)model;
	if (!caps)
		MORPH_RETURN(-EINVAL);
	memset(caps, 0, sizeof(*caps));
	caps->supports_generate = 1;
	caps->supports_edit = 1;
	caps->supports_multi_reference = 1;
	return 0;
}

static int volcengine_normalize_size(const struct model *model,
				     const char *size, char *out,
				     size_t out_cap)
{
	(void)model;
	if (!out || out_cap == 0)
		MORPH_RETURN(-EINVAL);
	if (!size || !*size)
		size = "2048x2048";
	if (strcmp(size, "2k") == 0 || strcmp(size, "4k") == 0) {
		snprintf(out, out_cap, "%s", size);
		return 0;
	}
	if (image_gen_validate_size(size) != 0)
		MORPH_RETURN(-EINVAL);
	snprintf(out, out_cap, "%s", size);
	return 0;
}

static int volcengine_build_request_meta(const struct model *model,
					 morph_buf_t *url,
					 morph_buf_t *auth)
{
	const char *path = "/images/generations";
	int rc;

	if (!model || !url || !auth)
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

static int volcengine_execute(struct model *model,
			      const struct image_request *request,
			      struct image_payload *payload)
{
	struct http_response resp = {0};
	struct arena *arena = NULL;
	morph_buf_t auth = {0};
	morph_buf_t url = {0};
	cJSON *body = NULL;
	char *body_text = NULL;
	const char *headers[1];
	int rc = 0;

	if (!model || !request || !request->prompt || !payload)
		MORPH_RETURN(-EINVAL);
	arena = arena_create(8192);
	if (!arena)
		MORPH_RETURN(-ENOMEM);
	body = cJSON_CreateObject();
	if (!body) {
		arena_destroy(arena);
		MORPH_RETURN(-ENOMEM);
	}
	cJSON_AddStringToObject(body, "model", model->model_id);
	cJSON_AddStringToObject(body, "prompt", request->prompt);
	cJSON_AddNumberToObject(body, "n", 1);
	cJSON_AddStringToObject(body, "size", request->size);
	cJSON_AddStringToObject(body, "response_format", "url");
	if (request->reference_image_count > 0) {
		cJSON *images = cJSON_CreateArray();

		if (!images) {
			cJSON_Delete(body);
			arena_destroy(arena);
			MORPH_RETURN(-ENOMEM);
		}
		for (int i = 0; i < request->reference_image_count; i++) {
			char *b64 = image_encode_base64(
				request->reference_images[i], 2048);
			cJSON *item;
			char *uri;
			size_t uri_len;

			if (!b64) {
				cJSON_Delete(images);
				cJSON_Delete(body);
				arena_destroy(arena);
				MORPH_RETURN(MORPH_ERR_FORMAT);
			}
			uri_len = strlen(b64) + 23;
			uri = arena_alloc(arena, uri_len);
			if (!uri) {
				free(b64);
				cJSON_Delete(images);
				cJSON_Delete(body);
				arena_destroy(arena);
				MORPH_RETURN(-ENOMEM);
			}
			snprintf(uri, uri_len, "data:image/png;base64,%s", b64);
			free(b64);
			item = cJSON_CreateString(uri);
			if (!item || !cJSON_AddItemToArray(images, item)) {
				cJSON_Delete(item);
				cJSON_Delete(images);
				cJSON_Delete(body);
				arena_destroy(arena);
				MORPH_RETURN(-ENOMEM);
			}
		}
		cJSON_AddItemToObject(body, "image", images);
	}
	body_text = cJSON_PrintUnformatted(body);
	cJSON_Delete(body);
	if (!body_text) {
		arena_destroy(arena);
		MORPH_RETURN(-ENOMEM);
	}
	rc = volcengine_build_request_meta(model, &url, &auth);
	if (rc != 0) {
		free(body_text);
		arena_destroy(arena);
		return rc;
	}
	headers[0] = morph_buf_cstr(&auth);
	rc = http_post_ex_timeout(morph_buf_cstr(&url), body_text,
				  strlen(body_text),
				  "application/json", headers, 1,
				  model->timeout_seconds, &resp);
	free(body_text);
	arena_destroy(arena);
	morph_buf_cleanup(&url);
	morph_buf_cleanup(&auth);
	if (rc != 0) {
		http_response_free(&resp);
		return rc;
	}
	if (resp.status_code != 200) {
		snprintf(model->last_error, sizeof(model->last_error),
			 "Volcengine image API returned HTTP %d: %.400s",
			 resp.status_code,
			 resp.body.data ? resp.body.data : "");
		http_response_free(&resp);
		MORPH_RETURN(MORPH_ERR_API);
	}
	rc = image_provider_parse_response(model, resp.body.data, payload);
	http_response_free(&resp);
	return rc;
}

const struct image_provider_ops *image_volcengine_provider(void)
{
	static const struct image_provider_ops ops = {
		.name = "volcengine-images",
		.capabilities = volcengine_capabilities,
		.normalize_size = volcengine_normalize_size,
		.execute = volcengine_execute,
	};

	return &ops;
}
