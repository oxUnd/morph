#ifndef MORPH_TERMINAL_TEXT_H
#define MORPH_TERMINAL_TEXT_H

#include <nb/buf.h>

#ifdef __cplusplus
extern "C" {
#endif

enum morph_terminal_text_mode {
	MORPH_TERMINAL_TEXT_MULTILINE = 0,
	MORPH_TERMINAL_TEXT_SINGLE_LINE = 1,
};

/* Stateful sanitizer for untrusted terminal text. It removes terminal escape
 * strings, preserves valid UTF-8 across chunks, and renders non-layout control
 * characters visibly. Keep one instance per independently streamed field. */
struct morph_terminal_sanitizer {
	int state;
	enum morph_terminal_text_mode mode;
	int pending_cr;
	unsigned char utf8_pending[4];
	size_t utf8_pending_len;
};

size_t morph_terminal_copy_sanitized_display_width(char *dst, size_t dst_cap, const char *src,
						   size_t max_width);
size_t morph_terminal_copy_ellipsized_display_width(char *dst, size_t dst_cap, const char *src,
						    size_t max_width, int keep_tail);
void morph_terminal_sanitizer_init(struct morph_terminal_sanitizer *sanitizer,
				   enum morph_terminal_text_mode mode);
void morph_terminal_sanitizer_reset(struct morph_terminal_sanitizer *sanitizer);
int morph_terminal_sanitize_feed(struct morph_terminal_sanitizer *sanitizer, nb_buf *out,
				 const char *src, size_t src_len, int finish);
char *morph_terminal_sanitize_dup(const char *src, size_t src_len,
				  enum morph_terminal_text_mode mode, size_t *out_len);

#ifdef __cplusplus
}
#endif
#endif
