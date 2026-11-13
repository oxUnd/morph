#ifndef IMAGE_PROVIDER_H
#define IMAGE_PROVIDER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "models/llm.h"
#include "nb/err.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
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
	/* payload starts empty; failure preserves it. Occupied: NB_ERR_EXISTS. */
	int (*execute)(struct model *model, const struct image_request *request,
		       struct image_payload *payload);
};

/* Immutable process-lifetime provider views; never free the returned tables. */
const struct image_provider_ops *image_openai_provider_borrow(void);
const struct image_provider_ops *image_volcengine_provider_borrow(void);
/* Both output buffers must be empty and distinct. Success fills both owners;
 * failure preserves both. Occupied output: NB_ERR_EXISTS. */
int image_provider_build_request_meta(const struct model *model, const char *path, nb_buf *url,
				      nb_buf *auth);
/* Borrows body for the call; provider request construction and ownership stay
 * with the caller. payload must be empty; failure leaves it unchanged.
 * label identifies the provider in HTTP error diagnostics. */
int image_provider_post_json(struct model *model, const char *path, const char *label,
			     const char *body, struct image_payload *payload);
/* Borrow request/model for this call. *body_out must be NULL; success returns an
 * owned body for cJSON_free. Failure preserves it; occupied output: NB_ERR_EXISTS. */
int image_volcengine_request_body_create(const struct model *model,
					 const struct image_request *request, char **body_out);

/* Borrow response_json for the call; payload must have no owned data. Success
 * creates an independent owner; failure preserves it. Occupied: NB_ERR_EXISTS. */
int image_provider_parse_response(struct model *model,
				  const char *response_json,
				  struct image_payload *payload);
void image_payload_cleanup(struct image_payload *payload);

#ifdef __cplusplus
}
#endif

#endif
