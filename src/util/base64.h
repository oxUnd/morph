#ifndef BASE64_H
#define BASE64_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

char *base64_encode(const unsigned char *data, size_t data_len);
#define MORPH_MEDIA_MAX_FILE_BYTES (64U * 1024U * 1024U)

char *base64_encode_file(const char *path);
/* Read in bounded chunks directly into the final prefixed Base64 buffer. */
int base64_encode_file_prefixed(const char *path, const char *prefix,
			       size_t max_bytes, char **out);
unsigned char *base64_decode(const char *text, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
