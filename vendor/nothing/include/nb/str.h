#ifndef NB_STR_H
#define NB_STR_H

#include "nb/base.h"
#include "nb/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A borrowed byte slice. It is not required to be NUL-terminated. */
typedef struct nb_str {
    const char *ptr;
    size_t len;
} nb_str;

#ifdef __cplusplus
#define NB_STR_LIT(s) (nb_str{(s), sizeof(s) - 1u})
#define NB_STR_NULL (nb_str{NULL, 0u})
#else
#define NB_STR_LIT(s) ((nb_str){ (s), sizeof(s) - 1u })
#define NB_STR_NULL   ((nb_str){ NULL, 0u })
#endif

/* Copies every byte, including embedded NUL, then appends a final NUL.
 * Failure leaves out unchanged. Overlapping source and destination are allowed. */
NB_EXPORT nb_status nb_str_copy_to(nb_str text, char *out, size_t cap);
/* Nonempty ASCII alphanumeric text with optional additional allowed bytes. */
NB_EXPORT bool nb_str_is_alnum(nb_str text, const char *punctuation);
NB_EXPORT bool nb_ascii_is_alnum(unsigned char byte);
NB_EXPORT bool nb_ascii_is_alnum_or(unsigned char byte, const char *punctuation);
NB_EXPORT nb_str nb_str_from_cstr(const char *s);
NB_EXPORT nb_str nb_str_from_parts(const char *ptr, size_t len);
NB_EXPORT bool nb_str_is_empty(nb_str s);
NB_EXPORT bool nb_str_eq(nb_str a, nb_str b);
NB_EXPORT bool nb_str_eq_cstr(nb_str a, const char *b);
NB_EXPORT int nb_str_cmp(nb_str a, nb_str b);

/* ASCII case-insensitive equality. Only 'A'-'Z' are folded; bytes >= 0x80
 * compare exactly. Lengths must match. */
NB_EXPORT bool nb_str_eq_icase(nb_str a, nb_str b);

NB_EXPORT bool nb_str_starts_with(nb_str s, nb_str prefix);
NB_EXPORT bool nb_str_ends_with(nb_str s, nb_str suffix);

/* Returns SIZE_MAX when not found. An empty needle matches at offset 0. */
NB_EXPORT size_t nb_str_find(nb_str haystack, nb_str needle);

/* Returns the offset of the first occurrence of c, or SIZE_MAX when absent.
 * An empty slice never contains c. */
NB_EXPORT size_t nb_str_find_char(nb_str s, char c);

/*
 * Zero-allocation delimiter splitter. The empty input yields one empty token;
 * "a,,b" yields "a", "", "b"; a trailing delimiter yields a final empty
 * token. Tokens borrow from the input.
 */
typedef struct nb_split_iter {
    nb_str rest;
    char delim;
    bool done;
    bool skip_empty;
} nb_split_iter;

NB_EXPORT void nb_split_init(nb_split_iter *it, nb_str s, char delim);
NB_EXPORT void nb_split_skip_empty(nb_split_iter *it, bool enabled);
NB_EXPORT bool nb_split_next(nb_split_iter *it, nb_str *token);

NB_EXPORT nb_str nb_str_trim(nb_str s);
NB_EXPORT nb_str nb_str_ltrim(nb_str s);
NB_EXPORT nb_str nb_str_rtrim(nb_str s);

NB_EXPORT bool nb_str_to_i64(nb_str s, int64_t *out);
NB_EXPORT bool nb_str_to_u64(nb_str s, uint64_t *out);
/* Parses the complete trimmed slice; embedded NUL bytes are rejected.
 * Uses strtod's current locale and accepted numeric syntax. */
NB_EXPORT bool nb_str_to_f64(nb_str s, double *out);

NB_EXPORT uint64_t nb_str_hash(nb_str s);
NB_EXPORT uint64_t nb_hash_bytes(const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* NB_STR_H */
