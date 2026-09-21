#ifndef IMAGE_UTIL_H
#define IMAGE_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define MORPH_IMAGE_MAX_DIMENSION 32768
#define MORPH_IMAGE_MAX_PIXELS (32U * 1024U * 1024U)

int image_pixel_bytes(int width, int height, int channels, size_t *bytes);
int image_validate_file(const char *path);
unsigned char *image_load_bounded(const char *path, int *width, int *height,
				 int *channels, int desired_channels);

struct image_encoded {
	char *base64;
	const char *mime_type;
};

int image_encode_base64(const char *path, int max_dim,
			struct image_encoded *encoded);
int image_encode_data_uri(const char *path, int max_dim, char **uri);
void image_encoded_cleanup(struct image_encoded *encoded);
int image_probe_size(const char *path, int *width, int *height);
int image_resize_file_exact(const char *path, int width, int height);
int image_gen_normalize_reference_size(int src_w, int src_h,
				       int *out_w, int *out_h);
int image_gen_format_size(char *buf, size_t buf_size, int width, int height);

#ifdef __cplusplus
}
#endif

#endif
