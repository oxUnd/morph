#include "image_provider.h"
#include "image_gen.h"
#include "util/cleanup.h"
#include "util/error.h"
#include "util/image_util.h"
#include "util/json.h"
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
		return -EINVAL;
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
		return -EINVAL;
	if (!size || !*size)
		size = "2048x2048";
	if (strcmp(size, "2k") == 0 || strcmp(size, "4k") == 0) {
		snprintf(out, out_cap, "%s", size);
		return 0;
	}
	if (image_gen_validate_size(size) != 0)
		return -EINVAL;
	snprintf(out, out_cap, "%s", size);
	return 0;
}

int image_volcengine_request_body_create(const struct model *model,
					 const struct image_request *request, char **body_out)
{
	cJSON *body = NULL;
	char *body_text = NULL;

	if (!model || !request || !request->prompt || !body_out)
		return -EINVAL;
	if (*body_out)
		return -EEXIST;
	body = cJSON_CreateObject();
	if (!body) {

		return -ENOMEM;
	}
	cJSON_AddStringToObject(body, "model", model->model_id);
	cJSON_AddStringToObject(body, "prompt", request->prompt);
	cJSON_AddNumberToObject(body, "n", 1);
	cJSON_AddStringToObject(body, "size", request->size);
	cJSON_AddStringToObject(body, "response_format", "url");
	cJSON_AddBoolToObject(body, "watermark", 0);
	if (request->reference_image_count > 0) {
		cJSON *images = cJSON_CreateArray();

		if (!images) {
			cJSON_Delete(body);

			return -ENOMEM;
		}
		for (int i = 0; i < request->reference_image_count; i++) {
			char *uri MORPH_CLEANUP(morph_charp_cleanup) = NULL;
			int encode_rc = image_encode_data_uri_copy(request->reference_images[i],
								   2048, &uri);

			if (encode_rc != 0) {
				cJSON_Delete(images);
				cJSON_Delete(body);

				return encode_rc;
			}
			cJSON *item = morph_json_string_take(&uri);
			if (!item || !cJSON_AddItemToArray(images, item)) {
				cJSON_Delete(item);
				cJSON_Delete(images);
				cJSON_Delete(body);

				return -ENOMEM;
			}
		}
		cJSON_AddItemToObject(body, "image", images);
	}
	body_text = cJSON_PrintUnformatted(body);
	cJSON_Delete(body);

	if (!body_text) {
		return -ENOMEM;
	}
	*body_out = body_text;
	return 0;
}

static int volcengine_execute(struct model *model,
			      const struct image_request *request,
			      struct image_payload *payload)
{
	char *body_text MORPH_CLEANUP(morph_json_text_cleanup) = NULL;
	int rc;

	if (!model || !request || !request->prompt || !payload)
		return -EINVAL;
	if (payload->data)
		return -EEXIST;
	rc = image_volcengine_request_body_create(model, request, &body_text);
	if (rc != 0)
		return rc;
	rc = image_provider_post_json(model, "/images/generations", "Volcengine image API",
				      body_text, payload);

	return rc;
}

const struct image_provider_ops *image_volcengine_provider_borrow(void)
{
	static const struct image_provider_ops ops = {
		.name = "volcengine-images",
		.capabilities = volcengine_capabilities,
		.normalize_size = volcengine_normalize_size,
		.execute = volcengine_execute,
	};

	return &ops;
}
