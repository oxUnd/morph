#ifndef MORPH_UTIL_UTF8_H
#define MORPH_UTIL_UTF8_H

#include "buf.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compatibility names for existing callers; all decoding uses utf8proc. */
typedef char utf8_int8_t;
typedef int32_t utf8_int32_t;

const char *utf8valid(const char *s);
const char *utf8nvalid(const char *s, size_t len);
size_t utf8len(const char *s);
size_t utf8nlen(const char *s, size_t len);
char *utf8dup(const char *s);
const char *utf8codepoint(const char *s, utf8_int32_t *cp);
/* Unicode categories grouped independently of the backing library. */
enum utf8_codepoint_class {
	UTF8_CLASS_OTHER,
	UTF8_CLASS_UPPER,
	UTF8_CLASS_LOWER,
	UTF8_CLASS_LETTER,
	UTF8_CLASS_MARK,
	UTF8_CLASS_NUMBER,
	UTF8_CLASS_SEPARATOR,
};

enum utf8_codepoint_class utf8_classify_codepoint(unsigned cp);

/* Conservative token-estimation policy, not an emoji-membership test. */
int utf8_is_emoji_component_cp(unsigned cp);
int utf8_is_continuation_byte(unsigned char byte);

size_t utf8_encode_codepoint(unsigned cp, char dst[4]);

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

size_t utf8_copy_ellipsized_display_width(char *dst, size_t dst_cap,
					  const char *src,
					  size_t max_width,
					  int keep_tail);

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

enum utf8_terminal_text_mode {
	UTF8_TERMINAL_TEXT_MULTILINE = 0,
	UTF8_TERMINAL_TEXT_SINGLE_LINE = 1,
};

/* Stateful sanitizer for untrusted terminal text. It removes terminal escape
 * strings, preserves valid UTF-8 across chunks, and renders non-layout control
 * characters visibly. Keep one instance per independently streamed field. */
struct utf8_terminal_sanitizer {
	int state;
	enum utf8_terminal_text_mode mode;
	int pending_cr;
	unsigned char utf8_pending[4];
	size_t utf8_pending_len;
};

void utf8_terminal_sanitizer_init(struct utf8_terminal_sanitizer *sanitizer,
				  enum utf8_terminal_text_mode mode);
void utf8_terminal_sanitizer_reset(struct utf8_terminal_sanitizer *sanitizer);
int utf8_terminal_sanitize_feed(struct utf8_terminal_sanitizer *sanitizer,
				morph_buf_t *out, const char *src,
				size_t src_len, int finish);
char *utf8_terminal_sanitize_dup(const char *src, size_t src_len,
			 enum utf8_terminal_text_mode mode,
			 size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* MORPH_UTIL_UTF8_H */
