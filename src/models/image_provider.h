#ifndef IMAGE_PROVIDER_H
#define IMAGE_PROVIDER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "models/llm.h"
#include <stddef.h>

enum image_payload_kind {
	IMAGE_PAYLOAD_URL,
	IMAGE_PAYLOAD_BASE64,
};

struct image_request {
	const char *prompt;
	const char *size;
	const char **reference_images;
	int reference_image_count;
	const char *mask_path;
};

struct image_payload {
	enum image_payload_kind kind;
	char *data;
	char output_format[16];
	struct model_usage usage;
	int has_usage;
};

struct image_capabilities {
	int supports_generate;
	int supports_edit;
	int supports_mask;
	int supports_multi_reference;
};

struct image_provider_ops {
	const char *name;
	int (*capabilities)(const struct model *model,
			    struct image_capabilities *caps);
	int (*normalize_size)(const struct model *model, const char *size,
			      char *out, size_t out_cap);
	int (*execute)(struct model *model, const struct image_request *request,
		       struct image_payload *payload);
};

const struct image_provider_ops *image_openai_provider(void);
const struct image_provider_ops *image_volcengine_provider(void);

int image_provider_parse_response(struct model *model,
				  const char *response_json,
				  struct image_payload *payload);
void image_payload_cleanup(struct image_payload *payload);

#ifdef __cplusplus
}
#endif

#endif
