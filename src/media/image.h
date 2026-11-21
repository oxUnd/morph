#ifndef IMAGE_UTIL_H
#define IMAGE_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <nb/str.h>

enum image_format {
	MORPH_IMAGE_UNKNOWN,
	MORPH_IMAGE_PNG,
	MORPH_IMAGE_JPEG,
	MORPH_IMAGE_WEBP,
	MORPH_IMAGE_GIF,
	MORPH_IMAGE_BMP,
};

enum image_format image_format_detect(const unsigned char *data, size_t len);
enum image_format image_format_from_mime(nb_str mime);
/* Immutable extension without a dot; NULL for unknown formats. */
const char *image_format_extension(enum image_format format);

#define MORPH_MEDIA_MAX_FILE_BYTES (64U * 1024U * 1024U)

/* Streams a bounded regular file into an encoded media payload. */
int image_file_payload_copy(const char *path, const char *prefix, size_t max_bytes, char **out);

#define MORPH_IMAGE_MAX_DIMENSION 32768
#define MORPH_IMAGE_MAX_PIXELS (32U * 1024U * 1024U)

int image_pixel_bytes(int width, int height, int channels, size_t *bytes);
int image_validate_file(const char *path);
/* Independent decoded pixels; release with stbi_image_free(). */
unsigned char *image_load_bounded_copy(const char *path, int *width, int *height, int *channels,
				       int desired_channels);

struct image_encoded {
	char *base64;
	const char *mime_type;
};

/* Independent encoded data. Output owners must be empty. Release with
 * image_encoded_cleanup() or free(*uri), respectively. */
int image_encode_base64_copy(const char *path, int max_dim, struct image_encoded *encoded);
int image_encode_data_uri_copy(const char *path, int max_dim, char **uri);
void image_encoded_cleanup(struct image_encoded *encoded);
int image_probe_size(const char *path, int *width, int *height);
/* Replaces a regular image atomically, preserving mode; rejects symlinks. */
int image_resize_file_exact(const char *path, int width, int height);
int image_gen_normalize_reference_size(int src_w, int src_h,
				       int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif

#endif
