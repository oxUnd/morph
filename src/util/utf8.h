#ifndef MORPH_UTIL_UTF8_H
#define MORPH_UTIL_UTF8_H

#include "sheredom_utf8.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t utf8_safe_len(const char *s, size_t max_bytes);

char *utf8_dup_clamped(const char *src, size_t max_bytes);

size_t utf8_sanitize_into(char *dst, const char *src, size_t src_len);

void utf8_sanitize_inplace(char *s);

int utf8_cp_width(unsigned cp);

size_t utf8_visible_len(const char *s);

/* Same as utf8_visible_len, but skips ANSI CSI escape sequences
 * (ESC '[' params final-byte) and OSC sequences (ESC ']' ... BEL or
 * ESC '\\'). Use this when the input string carries SGR colour codes
 * so the column count reflects what the terminal actually shows. */
size_t utf8_visible_len_ansi(const char *s);

const char *utf8_skip_forward(const char *s, size_t chars);

const char *utf8_skip_columns(const char *s, size_t cols);

size_t utf8_copy_vis(char *dst, size_t dst_cap, const char *src,
		      size_t max_vis);

int utf8_is_cjk_cp(unsigned cp);

size_t utf8_truncate(const char *s, size_t max_bytes);

int utf8_display_width(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* MORPH_UTIL_UTF8_H */
