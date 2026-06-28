#ifndef IMAGE_UTIL_H
#define IMAGE_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

char *image_encode_base64(const char *path, int max_dim);
int image_probe_size(const char *path, int *width, int *height);
int image_resize_file_exact(const char *path, int width, int height);
int image_gen_normalize_reference_size(int src_w, int src_h,
				       int *out_w, int *out_h);
int image_gen_format_size(char *buf, size_t buf_size, int width, int height);

#ifdef __cplusplus
}
#endif

#endif
