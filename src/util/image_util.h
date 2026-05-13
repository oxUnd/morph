#ifndef IMAGE_UTIL_H
#define IMAGE_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

char *image_encode_base64(const char *path, int max_dim);

#ifdef __cplusplus
}
#endif

#endif
