#ifndef NB_UTF8_H
#define NB_UTF8_H

#include "nb/base.h"
#include "nb/str.h"
#include "nb/buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Strict UTF-8 (RFC 3629) codec and code-point helpers.
 *
 * The rest of nbase stays byte-oriented, like PHP's zend_string and
 * lighttpd's buffer: ASCII-delimited search/split/INI operations are already
 * UTF-8 transparent. This module adds validation, code-point indexing and
 * boundary-safe trimming. Unicode properties and normalization use the pinned
 * Unicode 18 utf8proc backend; grapheme clustering remains caller policy.
 */

typedef enum nb_utf8_status {
    NB_UTF8_OK = 0,
    NB_UTF8_INVALID,     /* invalid lead byte or continuation byte */
    NB_UTF8_TRUNCATED,   /* input ends inside a sequence */
    NB_UTF8_OVERLONG,    /* non-shortest encoding */
    NB_UTF8_SURROGATE,   /* UTF-16 surrogate code point */
    NB_UTF8_TOO_LARGE    /* above U+10FFFF */
} nb_utf8_status;

#define NB_UTF8_BOM     "\xEF\xBB\xBF"
#define NB_UTF8_BOM_LEN 3u

/* Decodes one code point at *index. On success advances *index and writes
 * *cp. On failure *index is unchanged and *status (if non-NULL) receives the
 * reason. */
NB_EXPORT bool nb_utf8_decode(nb_str s, size_t *index, uint32_t *cp,
                              nb_utf8_status *status);

/* Returns NB_UTF8_OK, or the first error with its byte offset. */
NB_EXPORT nb_utf8_status nb_utf8_validate(nb_str s, size_t *error_offset);
NB_EXPORT bool nb_utf8_valid(nb_str s);

/* Number of code points; SIZE_MAX when the input is invalid. */
NB_EXPORT size_t nb_utf8_count(nb_str s);

/* Byte offset of the code point at index (0..count). SIZE_MAX when the index
 * is out of range or the input is invalid. */
NB_EXPORT size_t nb_utf8_offset(nb_str s, size_t codepoint_index);

/* Encodes cp into out[4]. Returns 1..4, or 0 for a surrogate or a value above
 * U+10FFFF. */
NB_EXPORT size_t nb_utf8_encode(uint32_t cp, char out[4]);

/* Borrowed slice by code-point index. Returns NB_STR_NULL when the input is
 * invalid or start_cp is past the end; count_cp is clamped to the end. */
NB_EXPORT nb_str nb_utf8_slice(nb_str s, size_t start_cp, size_t count_cp);

/* Unicode White_Space property. */
NB_EXPORT bool nb_utf8_is_space(uint32_t cp);

/* Trims Unicode White_Space on code-point boundaries. Invalid UTF-8 is left
 * unchanged: trimming stops at the first invalid byte. */
NB_EXPORT nb_str nb_utf8_trim(nb_str s);
NB_EXPORT nb_str nb_utf8_ltrim(nb_str s);
NB_EXPORT nb_str nb_utf8_rtrim(nb_str s);

/* Removes a leading UTF-8 BOM, if present. */
NB_EXPORT nb_str nb_utf8_strip_bom(nb_str s);

/* Display columns: 0 for controls/combining/format characters, 2 for East
 * Asian wide/fullwidth and common emoji, otherwise 1. Ambiguous-width
 * characters count as 1. Invalid code points return -1. */
NB_EXPORT int nb_utf8_width_cp(uint32_t cp);

/* Sums display columns, stopping at the first invalid byte. */
NB_EXPORT size_t nb_utf8_width(nb_str s);

/* Decodes one code point at *index and reports its width. */
NB_EXPORT bool nb_utf8_width_at(nb_str s, size_t *index, int *width);

/* Unicode 18 properties and bounded, forgiving text operations. Copy results
 * are malloc owners; release with free(). Views borrow the input. */
enum nb_utf8_class {
    NB_UTF8_CLASS_OTHER,
    NB_UTF8_CLASS_UPPER,
    NB_UTF8_CLASS_LOWER,
    NB_UTF8_CLASS_LETTER,
    NB_UTF8_CLASS_MARK,
    NB_UTF8_CLASS_NUMBER,
    NB_UTF8_CLASS_SEPARATOR,
};
/* Normalizes to NFC after full Unicode case folding. Appends to out. */
NB_EXPORT nb_status nb_utf8_casefold(nb_str text, nb_buf *out, nb_err *err);
NB_EXPORT const char *nb_utf8_next_cstr(const char *s, int32_t *cp);
NB_EXPORT int nb_utf8_is_continuation_byte(unsigned char byte);
NB_EXPORT enum nb_utf8_class nb_utf8_classify_codepoint(unsigned cp);
NB_EXPORT int nb_utf8_is_emoji_component_cp(unsigned cp);
NB_EXPORT size_t nb_utf8_clamp_bytes(const char *s, size_t max_bytes);
NB_EXPORT size_t nb_utf8_next_codepoint_len(const char *s, size_t avail);
NB_EXPORT int nb_utf8_read(const char *s, size_t avail, unsigned *cp, size_t *cp_len);
NB_EXPORT char *nb_utf8_ellipsize_bytes(const char *src, size_t max_bytes);
NB_EXPORT size_t nb_utf8_sanitize_into(char *dst, const char *src, size_t src_len);
NB_EXPORT void nb_utf8_sanitize_inplace(char *s);
NB_EXPORT size_t nb_utf8_copy_sanitized_clamped(char *dst, size_t dst_cap, const char *src,
                                                size_t max_bytes);
NB_EXPORT size_t nb_utf8_width_cstr(const char *s);
NB_EXPORT const char *nb_utf8_advance(const char *s, size_t chars);
NB_EXPORT const char *nb_utf8_prev(const char *start, const char *p);
NB_EXPORT int nb_utf8_is_unicode_space_cp(unsigned cp);

#ifdef __cplusplus
}
#endif

#endif /* NB_UTF8_H */
