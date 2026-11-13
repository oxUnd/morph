#include "image_provider.h"
#include "image_gen.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "media/image.h"
#include "cJSON.h"
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
		return NB_ERR_INVAL;
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
		return NB_ERR_INVAL;
	if (!size || !*size)
		size = "2048x2048";
	if (strcmp(size, "2k") == 0 || strcmp(size, "4k") == 0) {
		snprintf(out, out_cap, "%s", size);
		return 0;
	}
	if (image_gen_validate_size(size) != 0)
		return NB_ERR_INVAL;
	snprintf(out, out_cap, "%s", size);
	return 0;
}

int image_volcengine_request_body_create(const struct model *model,
					 const struct image_request *request, char **body_out)
{
	cJSON *body = NULL;
	char *body_text = NULL;

	if (!model || !request || !request->prompt || !body_out)
		return NB_ERR_INVAL;
	if (*body_out)
		return NB_ERR_EXISTS;
	body = cJSON_CreateObject();
	if (!body) {

		return NB_ERR_NOMEM;
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

			return NB_ERR_NOMEM;
		}
		for (int i = 0; i < request->reference_image_count; i++) {
			char *uri NB_CLEANUP(nb_charp_cleanup) = NULL;
			int encode_rc = image_encode_data_uri_copy(request->reference_images[i],
								   2048, &uri);

			if (encode_rc != 0) {
				cJSON_Delete(images);
				cJSON_Delete(body);

				return encode_rc;
			}
			cJSON *item = cJSON_CreateString(uri);
			if (item)
				nb_charp_cleanup(&uri);
			if (!item || !cJSON_AddItemToArray(images, item)) {
				cJSON_Delete(item);
				cJSON_Delete(images);
				cJSON_Delete(body);

				return NB_ERR_NOMEM;
			}
		}
		cJSON_AddItemToObject(body, "image", images);
	}
	body_text = cJSON_PrintUnformatted(body);
	cJSON_Delete(body);

	if (!body_text) {
		return NB_ERR_NOMEM;
	}
	*body_out = body_text;
	return 0;
}

static int volcengine_execute(struct model *model,
			      const struct image_request *request,
			      struct image_payload *payload)
{
	char *body_text NB_CLEANUP(cJSON_FreeTextOwner) = NULL;
	int rc;

	if (!model || !request || !request->prompt || !payload)
		return NB_ERR_INVAL;
	if (payload->data)
		return NB_ERR_EXISTS;
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
