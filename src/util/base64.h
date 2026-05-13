#ifndef BASE64_H
#define BASE64_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

char *base64_encode(const unsigned char *data, size_t data_len);
char *base64_encode_file(const char *path);

#ifdef __cplusplus
}
#endif

#endif
