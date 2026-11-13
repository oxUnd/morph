#define _GNU_SOURCE
#include <morph/tui/terminal/text.h>
#include "nb/cleanup.h"
#include "nb/err.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static size_t nb_utf8_prefix_len(const char *s)
{
	if (!s)
		return 0;
	unsigned char lead = (unsigned char)*s;
	int size = lead < 0x80			  ? 1
		   : lead >= 0xc2 && lead <= 0xdf ? 2
		   : lead >= 0xe0 && lead <= 0xef ? 3
		   : lead >= 0xf0 && lead <= 0xf4 ? 4
						  : 1;
	return size > 0 ? (size_t)size : 1;
}
enum terminal_sanitize_state {
	TERMINAL_SANITIZE_TEXT = 0,
	TERMINAL_SANITIZE_ESC,
	TERMINAL_SANITIZE_CSI,
	TERMINAL_SANITIZE_OSC,
	TERMINAL_SANITIZE_OSC_ESC,
	TERMINAL_SANITIZE_STRING,
	TERMINAL_SANITIZE_STRING_ESC,
};

size_t morph_terminal_display_width_ansi(const char *s)
{
	if (!s)
		return 0;
	size_t n = 0;
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		if (*p == 0x1B && p[1] == '[') {
			/* CSI: ESC '[' params (0x30-0x3F) intermediates
			 * (0x20-0x2F)* final (0x40-0x7E). */
			p += 2;
			while (*p && *p < 0x40)
				p++;
			if (*p)
				p++;
			continue;
		}
		if (*p == 0x1B && p[1] == ']') {
			/* OSC: ESC ']' ... terminated by BEL (0x07) or
			 * ST (ESC '\\'). */
			p += 2;
			while (*p) {
				if (*p == 0x07) {
					p++;
					break;
				}
				if (*p == 0x1B && p[1] == '\\') {
					p += 2;
					break;
				}
				p++;
			}
			continue;
		}
		if (*p == 0x1B && p[1]) {
			/* Other 2-byte escape (ESC + final). Skip both. */
			p += 2;
			continue;
		}
		int32_t cp;
		const char *next = nb_utf8_next_cstr((const char *)p, &cp);
		n += (size_t)nb_utf8_width_cp((unsigned)cp);
		p = (const unsigned char *)next;
	}
	return n;
}

const char *morph_terminal_advance_display_width_borrow(const char *s, size_t cols)
{
	size_t vis = 0;

	if (!s)
		return NULL;
	while (*s && vis < cols) {
		int32_t cp;
		const char *next = nb_utf8_next_cstr(s, &cp);
		size_t w = (size_t)nb_utf8_width_cp((unsigned)cp);

		if (vis + w > cols)
			break;
		vis += w;
		s = next;
	}
	return s;
}

const char *morph_terminal_suffix_display_width_borrow(const char *s, size_t cols)
{
	const char *start;
	const char *p;
	size_t total;
	size_t drop;

	if (!s)
		return NULL;
	total = nb_utf8_width_cstr(s);
	if (total <= cols)
		return s;
	drop = total - cols;
	start = s;
	p = s;
	while (*p) {
		int32_t cp;
		const char *next = nb_utf8_next_cstr(p, &cp);
		size_t w = (size_t)nb_utf8_width_cp((unsigned)cp);

		if (drop < w)
			break;
		drop -= w;
		start = next;
		p = next;
	}
	return start;
}

size_t morph_terminal_copy_display_width(char *dst, size_t dst_cap, const char *src,
					 size_t max_width)
{
	size_t written = 0;
	size_t vis = 0;
	if (!dst || dst_cap == 0)
		return 0;
	if (!src) {
		dst[0] = '\0';
		return 0;
	}
	while (*src) {
		int32_t cp;
		const char *next = nb_utf8_next_cstr(src, &cp);
		size_t cb = (size_t)(next - src);
		size_t w = (size_t)nb_utf8_width_cp((unsigned)cp);
		if (vis + w > max_width)
			break;
		if (written + cb >= dst_cap)
			break;
		memcpy(dst + written, src, cb);
		written += cb;
		src += cb;
		vis += w;
	}
	dst[written] = '\0';
	return vis;
}

size_t morph_terminal_copy_sanitized_display_width(char *dst, size_t dst_cap, const char *src,
						   size_t max_width)
{
	size_t written = 0;
	size_t vis = 0;
	size_t avail;

	if (!dst || dst_cap == 0)
		return 0;
	if (!src) {
		dst[0] = '\0';
		return 0;
	}
	avail = strlen(src);
	while (*src) {
		unsigned cp;
		size_t cb;
		size_t w;

		if (!nb_utf8_read(src, avail, &cp, &cb)) {
			src++;
			avail--;
			continue;
		}
		w = (size_t)nb_utf8_width_cp(cp);
		if (vis + w > max_width)
			break;
		if (written + cb >= dst_cap)
			break;
		memcpy(dst + written, src, cb);
		written += cb;
		src += cb;
		avail -= cb;
		vis += w;
	}
	dst[written] = '\0';
	return vis;
}

size_t morph_terminal_copy_ellipsized_display_width(char *dst, size_t dst_cap, const char *src,
						    size_t max_width, int keep_tail)
{
	static const char ellipsis[] = "…";
	size_t width;
	size_t ellipsis_bytes;
	size_t copied;
	const char *start;

	if (!dst || dst_cap == 0)
		return 0;
	dst[0] = '\0';
	if (!src || max_width == 0)
		return 0;
	width = nb_utf8_width_cstr(src);
	if (width <= max_width)
		return morph_terminal_copy_sanitized_display_width(dst, dst_cap, src, max_width);
	if (max_width == 1) {
		(void)morph_terminal_copy_display_width(dst, dst_cap, ellipsis, 1);
		return 1;
	}
	ellipsis_bytes = strlen(ellipsis);
	if (dst_cap <= ellipsis_bytes)
		return morph_terminal_copy_sanitized_display_width(dst, dst_cap, src, max_width);
	if (!keep_tail) {
		size_t used_bytes;

		copied = morph_terminal_copy_sanitized_display_width(dst, dst_cap, src,
								     max_width - 1);
		used_bytes = strlen(dst);
		if (copied < max_width && used_bytes + ellipsis_bytes < dst_cap) {
			memcpy(dst + used_bytes, ellipsis, ellipsis_bytes + 1);
			copied++;
		}
		return copied;
	}
	memcpy(dst, ellipsis, ellipsis_bytes);
	dst[ellipsis_bytes] = '\0';
	start = morph_terminal_suffix_display_width_borrow(src, max_width - 1);
	copied = morph_terminal_copy_sanitized_display_width(
		dst + ellipsis_bytes, dst_cap - ellipsis_bytes, start, max_width - 1);
	return copied + 1;
}

size_t morph_terminal_strip_ansi(char *dst, const char *src, size_t src_len)
{
	size_t j = 0;

	if (!dst || !src)
		return 0;
	for (size_t i = 0; i < src_len;) {
		unsigned char c = (unsigned char)src[i];

		if (c == 0x1B && i + 1 < src_len && src[i + 1] == '[') {
			i += 2;
			while (i < src_len && (unsigned char)src[i] < 0x40)
				i++;
			if (i < src_len)
				i++;
			continue;
		}
		if (c == 0x1B && i + 1 < src_len && src[i + 1] == ']') {
			i += 2;
			while (i < src_len) {
				if (src[i] == '\a') {
					i++;
					break;
				}
				if (src[i] == '\033' && i + 1 < src_len && src[i + 1] == '\\') {
					i += 2;
					break;
				}
				i++;
			}
			continue;
		}
		if (c == 0x1B && i + 1 < src_len && src[i + 1] == '_') {
			i += 2;
			while (i < src_len) {
				if (src[i] == '\033' && i + 1 < src_len && src[i + 1] == '\\') {
					i += 2;
					break;
				}
				i++;
			}
			continue;
		}
		if (c == 0x1B && i + 1 < src_len) {
			i += 2;
			continue;
		}
		dst[j++] = src[i++];
	}
	dst[j] = '\0';
	return j;
}

char *morph_terminal_strip_ansi_dup(const char *src, size_t src_len, size_t *out_len)
{
	char *out;
	size_t len;

	if (!src)
		return NULL;
	out = malloc(src_len + 1);
	if (!out)
		return NULL;
	len = morph_terminal_strip_ansi(out, src, src_len);
	if (out_len)
		*out_len = len;
	return out;
}

static int terminal_append_visible_control(nb_buf *out, unsigned cp)
{
	if (cp == '\b')
		return (nb_buf_append_cstr(out, "\\b") ? 0 : NB_ERR_NOMEM);
	if (cp == '\a')
		return (nb_buf_append_cstr(out, "\\a") ? 0 : NB_ERR_NOMEM);
	if (cp <= 0xffu)
		return (nb_buf_printf(out, "\\x%02x", cp) ? 0 : NB_ERR_NOMEM);
	return (nb_buf_printf(out, "\\u%04x", cp) ? 0 : NB_ERR_NOMEM);
}

static int terminal_append_text(struct morph_terminal_sanitizer *sanitizer, nb_buf *out,
				const char *text, size_t len, unsigned cp)
{
	if (cp == '\n') {
		if (sanitizer->mode == MORPH_TERMINAL_TEXT_SINGLE_LINE)
			return (nb_buf_append_char(out, ' ') ? 0 : NB_ERR_NOMEM);
		return (nb_buf_append_char(out, '\n') ? 0 : NB_ERR_NOMEM);
	}
	if (cp == '\t') {
		if (sanitizer->mode == MORPH_TERMINAL_TEXT_SINGLE_LINE)
			return (nb_buf_append_char(out, ' ') ? 0 : NB_ERR_NOMEM);
		return (nb_buf_append_char(out, '\t') ? 0 : NB_ERR_NOMEM);
	}
	if (cp < 0x20u || (cp >= 0x7fu && cp <= 0x9fu))
		return terminal_append_visible_control(out, cp);
	return (nb_buf_append(out, text, len) ? 0 : NB_ERR_NOMEM);
}

static int terminal_flush_cr(struct morph_terminal_sanitizer *sanitizer, nb_buf *out)
{
	if (!sanitizer->pending_cr)
		return 0;
	sanitizer->pending_cr = 0;
	if (sanitizer->mode == MORPH_TERMINAL_TEXT_SINGLE_LINE)
		return (nb_buf_append_char(out, ' ') ? 0 : NB_ERR_NOMEM);
	return (nb_buf_append_cstr(out, "\\r") ? 0 : NB_ERR_NOMEM);
}

static int terminal_process_text_byte(struct morph_terminal_sanitizer *sanitizer, nb_buf *out,
				      const char *src, size_t src_len, size_t *index)
{
	unsigned char c = (unsigned char)src[*index];
	unsigned cp;
	size_t expected;
	size_t cp_len;
	int rc;

	if (sanitizer->pending_cr) {
		if (c == '\n') {
			sanitizer->pending_cr = 0;
			(*index)++;
			return sanitizer->mode == MORPH_TERMINAL_TEXT_SINGLE_LINE
				       ? (nb_buf_append_char(out, ' ') ? NB_OK : NB_ERR_NOMEM)
				       : (nb_buf_append_char(out, '\n') ? NB_OK : NB_ERR_NOMEM);
		}
		rc = terminal_flush_cr(sanitizer, out);
		if (rc != 0)
			return rc;
	}
	if (c == '\r') {
		sanitizer->pending_cr = 1;
		(*index)++;
		return 0;
	}
	if (c < 0x80u) {
		(*index)++;
		return terminal_append_text(sanitizer, out, (const char *)&src[*index - 1], 1, c);
	}
	expected = (size_t)nb_utf8_prefix_len(src + *index);
	if (expected > 1 && expected <= sizeof(sanitizer->utf8_pending) &&
	    src_len - *index < expected) {
		sanitizer->utf8_pending_len = src_len - *index;
		memcpy(sanitizer->utf8_pending, src + *index, sanitizer->utf8_pending_len);
		*index = src_len;
		return 0;
	}
	cp_len = nb_utf8_next_codepoint_len(src + *index, src_len - *index);
	if (cp_len == 0) {
		(*index)++;
		return 0;
	}
	if (!nb_utf8_read(src + *index, cp_len, &cp, &cp_len)) {
		(*index)++;
		return 0;
	}
	rc = terminal_append_text(sanitizer, out, src + *index, cp_len, cp);
	*index += cp_len;
	return rc;
}

static int terminal_process_pending_utf8(struct morph_terminal_sanitizer *sanitizer, nb_buf *out,
					 const char **src, size_t *src_len, int finish)
{
	while (sanitizer->utf8_pending_len) {
		unsigned char combined[4];
		size_t saved = sanitizer->utf8_pending_len;
		size_t expected = nb_utf8_prefix_len((const char *)sanitizer->utf8_pending);
		size_t needed = expected > saved ? expected - saved : 0;
		size_t taken = *src_len < needed ? *src_len : needed;

		if (expected > sizeof(combined) || saved > sizeof(combined)) {
			sanitizer->utf8_pending_len = 0;
			return 0;
		}
		memcpy(combined, sanitizer->utf8_pending, saved);
		if (taken)
			memcpy(combined + saved, *src, taken);
		*src += taken;
		*src_len -= taken;
		size_t count = saved + taken;
		if (count < expected && !finish) {
			memcpy(sanitizer->utf8_pending, combined, count);
			sanitizer->utf8_pending_len = count;
			return 0;
		}
		sanitizer->utf8_pending_len = 0;
		/* Reprocess malformed tails so valid bytes after a bad prefix survive. */
		size_t skip = count < expected ? 1 : 0;
		int rc = morph_terminal_sanitize_feed(sanitizer, out, (const char *)combined + skip,
						      count - skip, finish && *src_len == 0);
		if (rc < 0)
			return rc;
		if (!*src_len)
			return 0;
	}
	return 0;
}

void morph_terminal_sanitizer_init(struct morph_terminal_sanitizer *sanitizer,
				   enum morph_terminal_text_mode mode)
{
	if (!sanitizer)
		return;
	memset(sanitizer, 0, sizeof(*sanitizer));
	sanitizer->mode = mode;
}

void morph_terminal_sanitizer_reset(struct morph_terminal_sanitizer *sanitizer)
{
	enum morph_terminal_text_mode mode;

	if (!sanitizer)
		return;
	mode = sanitizer->mode;
	memset(sanitizer, 0, sizeof(*sanitizer));
	sanitizer->mode = mode;
}

int morph_terminal_sanitize_feed(struct morph_terminal_sanitizer *sanitizer, nb_buf *out,
				 const char *src, size_t src_len, int finish)
{
	size_t i = 0;
	int rc;

	if (!sanitizer || !out || (!src && src_len > 0))
		return NB_ERR_INVAL;
	if (!src)
		src = "";
	rc = terminal_process_pending_utf8(sanitizer, out, &src, &src_len, finish);
	if (rc != 0)
		return rc;
	while (i < src_len) {
		unsigned char c = (unsigned char)src[i];

		switch (sanitizer->state) {
		case TERMINAL_SANITIZE_TEXT:
			if (sanitizer->pending_cr &&
			    (c == 0x1bu || c == 0x90u || c == 0x98u || c == 0x9bu || c == 0x9du ||
			     c == 0x9eu || c == 0x9fu)) {
				rc = terminal_flush_cr(sanitizer, out);
				if (rc != 0)
					return rc;
			}
			if (c == 0x1bu) {
				sanitizer->state = TERMINAL_SANITIZE_ESC;
				i++;
			} else if (c == 0x9bu) {
				sanitizer->state = TERMINAL_SANITIZE_CSI;
				i++;
			} else if (c == 0x9du) {
				sanitizer->state = TERMINAL_SANITIZE_OSC;
				i++;
			} else if (c == 0x90u || c == 0x98u || c == 0x9eu || c == 0x9fu) {
				sanitizer->state = TERMINAL_SANITIZE_STRING;
				i++;
			} else {
				rc = terminal_process_text_byte(sanitizer, out, src, src_len, &i);
				if (rc != 0)
					return rc;
			}
			break;
		case TERMINAL_SANITIZE_ESC:
			if (c == '[')
				sanitizer->state = TERMINAL_SANITIZE_CSI;
			else if (c == ']')
				sanitizer->state = TERMINAL_SANITIZE_OSC;
			else if (c == 'P' || c == 'X' || c == '^' || c == '_')
				sanitizer->state = TERMINAL_SANITIZE_STRING;
			else if (c == 0x1bu)
				sanitizer->state = TERMINAL_SANITIZE_ESC;
			else
				sanitizer->state = TERMINAL_SANITIZE_TEXT;
			i++;
			break;
		case TERMINAL_SANITIZE_CSI:
			if (c >= 0x40u && c <= 0x7eu)
				sanitizer->state = TERMINAL_SANITIZE_TEXT;
			else if (c == 0x1bu)
				sanitizer->state = TERMINAL_SANITIZE_ESC;
			i++;
			break;
		case TERMINAL_SANITIZE_OSC:
			if (c == '\a' || c == 0x9cu)
				sanitizer->state = TERMINAL_SANITIZE_TEXT;
			else if (c == 0x1bu)
				sanitizer->state = TERMINAL_SANITIZE_OSC_ESC;
			i++;
			break;
		case TERMINAL_SANITIZE_OSC_ESC:
			if (c == '\\')
				sanitizer->state = TERMINAL_SANITIZE_TEXT;
			else if (c != 0x1bu)
				sanitizer->state = TERMINAL_SANITIZE_OSC;
			i++;
			break;
		case TERMINAL_SANITIZE_STRING:
			if (c == 0x9cu)
				sanitizer->state = TERMINAL_SANITIZE_TEXT;
			else if (c == 0x1bu)
				sanitizer->state = TERMINAL_SANITIZE_STRING_ESC;
			i++;
			break;
		case TERMINAL_SANITIZE_STRING_ESC:
			if (c == '\\')
				sanitizer->state = TERMINAL_SANITIZE_TEXT;
			else if (c != 0x1bu)
				sanitizer->state = TERMINAL_SANITIZE_STRING;
			i++;
			break;
		default:
			morph_terminal_sanitizer_reset(sanitizer);
			break;
		}
	}
	if (finish) {
		const char *empty = "";
		size_t remaining = 0;

		rc = terminal_process_pending_utf8(sanitizer, out, &empty, &remaining, 1);
		if (rc != 0)
			return rc;
		rc = terminal_flush_cr(sanitizer, out);
		morph_terminal_sanitizer_reset(sanitizer);
		if (rc != 0)
			return rc;
	}
	return 0;
}

char *morph_terminal_sanitize_dup(const char *src, size_t src_len,
				  enum morph_terminal_text_mode mode, size_t *out_len)
{
	struct morph_terminal_sanitizer sanitizer;
	nb_buf out NB_CLEANUP(nb_buf_destroy) = {0};
	char *result;
	int rc;

	if (!src)
		return NULL;
	if (src_len > SIZE_MAX - 16)
		return NULL;
	rc = nb_buf_reserve(&out, src_len + 16) ? NB_OK : NB_ERR_NOMEM;
	if (rc != 0)
		return NULL;
	morph_terminal_sanitizer_init(&sanitizer, mode);
	rc = morph_terminal_sanitize_feed(&sanitizer, &out, src, src_len, 1);
	if (rc != 0) {
		return NULL;
	}
	if (out_len)
		*out_len = out.len;
	result = (nb_buf_ok(&out) ? strdup(nb_buf_cstr(&out)) : NULL);
	return result;
}
