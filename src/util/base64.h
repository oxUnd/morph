#ifndef BASE64_H
#define BASE64_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* Encoders/decoder return independent malloc owners; release with free(). */
char *base64_encode_copy(const unsigned char *data, size_t data_len);
#define MORPH_MEDIA_MAX_FILE_BYTES (64U * 1024U * 1024U)

char *base64_encode_file_copy(const char *path);
/* Read in bounded chunks directly into the final prefixed Base64 buffer. */
/* out must be NULL; failure preserves it and releases partial construction. */
int base64_encode_file_prefixed_copy(const char *path, const char *prefix, size_t max_bytes,
				     char **out);
unsigned char *base64_decode_copy(const char *text, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
