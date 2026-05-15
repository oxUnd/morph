#ifndef MORPH_UTIL_UTF8_H
#define MORPH_UTIL_UTF8_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Find a valid UTF-8 character boundary at or before max_bytes.
 * UTF-8 continuation bytes are 0x80..0xBF; lead bytes are ASCII (<0x80) or 0xC0+.
 * Returns the largest length <= max_bytes that does not split a multi-byte
 * sequence. If s is NULL, returns 0. */
size_t utf8_safe_len(const char *s, size_t max_bytes);

/* Duplicate src into a freshly malloc'd buffer no larger than max_bytes
 * (excluding trailing NUL), keeping UTF-8 sequences intact. Returns NULL on
 * allocation failure. When truncation occurs, "…(truncated)" is appended.
 * Caller must free() the returned pointer. */
char *utf8_dup_clamped(const char *src, size_t max_bytes);

/* Append the UTF-8 encoding of src into dst, dropping any malformed byte
 * sequences and rejecting embedded NULs. dst must point to a buffer of at
 * least src_len bytes. Returns the number of bytes written. */
size_t utf8_sanitize_into(char *dst, const char *src, size_t src_len);

#ifdef __cplusplus
}
#endif

#endif /* MORPH_UTIL_UTF8_H */
