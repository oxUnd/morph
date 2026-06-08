#ifndef MORPH_UTIL_UTF8_H
#define MORPH_UTIL_UTF8_H

#include "sheredom_utf8.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t utf8_clamp_bytes(const char *s, size_t max_bytes);

size_t utf8_next_codepoint_len(const char *s, size_t avail);

int utf8_decode_codepoint(const char *s, size_t avail, unsigned *cp,
			  size_t *cp_len);

char *utf8_dup_clamped(const char *src, size_t max_bytes);

size_t utf8_sanitize_into(char *dst, const char *src, size_t src_len);

void utf8_sanitize_inplace(char *s);

size_t utf8_copy_sanitized_clamped(char *dst, size_t dst_cap,
				   const char *src, size_t max_bytes);

int utf8_codepoint_width(unsigned cp);

size_t utf8_display_width(const char *s);

/* Same as utf8_display_width, but skips ANSI CSI escape sequences
 * (ESC '[' params final-byte) and OSC sequences (ESC ']' ... BEL or
 * ESC '\\'). Use this when the input string carries SGR colour codes
 * so the column count reflects what the terminal actually shows. */
size_t utf8_display_width_ansi(const char *s);

const char *utf8_advance_codepoints(const char *s, size_t chars);

const char *utf8_advance_display_width(const char *s, size_t cols);

const char *utf8_suffix_display_width(const char *s, size_t cols);

size_t utf8_copy_display_width(char *dst, size_t dst_cap,
			       const char *src, size_t max_width);

size_t utf8_copy_sanitized_display_width(char *dst, size_t dst_cap,
					 const char *src,
					 size_t max_width);

const char *utf8_prev_codepoint(const char *start, const char *p);

int utf8_is_cjk_cp(unsigned cp);
int utf8_is_hiragana_cp(unsigned cp);
int utf8_is_katakana_cp(unsigned cp);
int utf8_is_hangul_cp(unsigned cp);

int utf8_is_cjk_sentence_punct(unsigned cp);

int utf8_is_fullwidth_pipe_cp(unsigned cp);
int utf8_is_fullwidth_dash_cp(unsigned cp);
int utf8_is_fullwidth_colon_cp(unsigned cp);

int utf8_is_unicode_space_cp(unsigned cp);
int utf8_is_latin_extended_cp(unsigned cp);

size_t utf8_strip_ansi(char *dst, const char *src, size_t src_len);
char *utf8_strip_ansi_dup(const char *src, size_t src_len,
			  size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* MORPH_UTIL_UTF8_H */
