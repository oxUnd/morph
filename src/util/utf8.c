#define _GNU_SOURCE

#include "utf8.h"
#include "utf8proc/utf8proc.h"

#include <stdlib.h>
#include <string.h>

/* Bound every iterator call, including non-terminated source chunks. */
static utf8proc_ssize_t utf8_iterate_bytes(const char *s, size_t len,
					utf8proc_int32_t *cp)
{
	if (len > 4)
		len = 4;
	return utf8proc_iterate((const utf8proc_uint8_t *)s,
			       (utf8proc_ssize_t)len, cp);
}

const char *utf8nvalid(const char *s, size_t len)
{
	if (!s)
		return NULL;
	for (size_t pos = 0; pos < len && s[pos];) {
		utf8proc_int32_t cp;
		utf8proc_ssize_t count = utf8_iterate_bytes(s + pos, len - pos, &cp);

		if (count < 1)
			return s + pos;
		pos += (size_t)count;
	}
	return NULL;
}

const char *utf8valid(const char *s)
{
	return utf8nvalid(s, s ? strlen(s) : 0);
}

size_t utf8nlen(const char *s, size_t len)
{
	size_t count = 0;

	if (!s)
		return 0;
	for (size_t pos = 0; pos < len && s[pos]; count++) {
		utf8proc_int32_t cp;
		utf8proc_ssize_t bytes = utf8_iterate_bytes(s + pos, len - pos, &cp);

		pos += bytes > 0 ? (size_t)bytes : 1;
	}
	return count;
}

size_t utf8len(const char *s)
{
	return utf8nlen(s, s ? strlen(s) : 0);
}

char *utf8dup(const char *s)
{
	return s ? strdup(s) : NULL;
}

const char *utf8codepoint(const char *s, utf8_int32_t *cp)
{
	if (!s || !cp)
		return s;
	utf8proc_ssize_t count = utf8proc_iterate((const utf8proc_uint8_t *)s, -1, cp);
	if (count < 1) {
		*cp = 0xfffd;
		return s + 1;
	}
	return s + count;
}

static size_t utf8_prefix_len(const char *s)
{
	if (!s)
		return 0;
	int size = utf8proc_utf8class[(unsigned char)*s];
	return size > 0 ? (size_t)size : 1;
}

int utf8_is_continuation_byte(unsigned char byte)
{
	return byte >= 0x80u && byte < 0xc0u && utf8proc_utf8class[byte] == 0;
}

enum utf8_codepoint_class utf8_classify_codepoint(unsigned cp)
{
	if (cp > INT32_MAX)
		return UTF8_CLASS_OTHER;
	switch (utf8proc_category((utf8proc_int32_t)cp)) {
	case UTF8PROC_CATEGORY_LU:
	case UTF8PROC_CATEGORY_LT:
		return UTF8_CLASS_UPPER;
	case UTF8PROC_CATEGORY_LL:
		return UTF8_CLASS_LOWER;
	case UTF8PROC_CATEGORY_LM:
	case UTF8PROC_CATEGORY_LO:
		return UTF8_CLASS_LETTER;
	case UTF8PROC_CATEGORY_MN:
	case UTF8PROC_CATEGORY_MC:
	case UTF8PROC_CATEGORY_ME:
		return UTF8_CLASS_MARK;
	case UTF8PROC_CATEGORY_ND:
	case UTF8PROC_CATEGORY_NL:
	case UTF8PROC_CATEGORY_NO:
		return UTF8_CLASS_NUMBER;
	case UTF8PROC_CATEGORY_ZS:
	case UTF8PROC_CATEGORY_ZL:
	case UTF8PROC_CATEGORY_ZP:
		return UTF8_CLASS_SEPARATOR;
	default:
		return UTF8_CLASS_OTHER;
	}
}

int utf8_is_emoji_component_cp(unsigned cp)
{
	if (cp > INT32_MAX)
		return 0;
	const utf8proc_property_t *property = utf8proc_get_property((utf8proc_int32_t)cp);
	return property->boundclass == UTF8PROC_BOUNDCLASS_EXTENDED_PICTOGRAPHIC ||
	       property->boundclass == UTF8PROC_BOUNDCLASS_REGIONAL_INDICATOR ||
	       property->boundclass == UTF8PROC_BOUNDCLASS_ZWJ ||
	       (property->boundclass == UTF8PROC_BOUNDCLASS_EXTEND &&
		(property->ignorable || property->charwidth == 2));
}

size_t utf8_encode_codepoint(unsigned cp, char dst[4])
{
	if (!dst || cp > INT32_MAX || !utf8proc_codepoint_valid((utf8proc_int32_t)cp))
		return 0;
	return (size_t)utf8proc_encode_char((utf8proc_int32_t)cp, (utf8proc_uint8_t *)dst);
}

size_t utf8_clamp_bytes(const char *s, size_t max_bytes)
{
	size_t pos = 0;

	if (!s)
		return 0;
	size_t len = strnlen(s, max_bytes);
	while (pos < len) {
		utf8proc_int32_t cp;
		utf8proc_ssize_t count = utf8_iterate_bytes(s + pos, len - pos, &cp);

		if (count < 1)
			break;
		pos += (size_t)count;
	}
	return pos;
}

size_t utf8_next_codepoint_len(const char *s, size_t avail)
{
	utf8proc_int32_t cp;

	if (!s || !avail || !*s)
		return 0;
	utf8proc_ssize_t count = utf8_iterate_bytes(s, avail, &cp);
	return count > 0 ? (size_t)count : 1;
}

int utf8_decode_codepoint(const char *s, size_t avail, unsigned *cp,
			  size_t *cp_len)
{
	utf8proc_int32_t value;

	if (!s || !avail || !*s || !cp || !cp_len)
		return 0;
	utf8proc_ssize_t count = utf8_iterate_bytes(s, avail, &value);
	if (count < 1)
		return 0;
	*cp = (unsigned)value;
	*cp_len = (size_t)count;
	return 1;
}

char *utf8_dup_clamped(const char *src, size_t max_bytes)
{
	if (!src)
		return NULL;
	size_t len = strlen(src);
	if (len <= max_bytes) {
		return utf8dup(src);
	}
	static const char marker[] = "\xE2\x80\xA6(truncated)";
	const size_t marker_len = sizeof(marker) - 1;
	if (max_bytes <= marker_len) {
		size_t cut = utf8_clamp_bytes(src, max_bytes);
		char *dup = malloc(cut + 1);
		if (!dup)
			return NULL;
		memcpy(dup, src, cut);
		dup[cut] = '\0';
		return dup;
	}
	size_t cut = utf8_clamp_bytes(src, max_bytes - marker_len);
	char *dup = malloc(cut + marker_len + 1);
	if (!dup)
		return NULL;
	memcpy(dup, src, cut);
	memcpy(dup + cut, marker, marker_len);
	dup[cut + marker_len] = '\0';
	return dup;
}

size_t utf8_sanitize_into(char *dst, const char *src, size_t src_len)
{
	size_t written = 0;

	if (!dst || !src)
		return 0;
	for (size_t pos = 0; pos < src_len;) {
		utf8proc_int32_t cp;
		utf8proc_ssize_t count = utf8_iterate_bytes(src + pos, src_len - pos, &cp);

		if (count < 1 || cp == 0) {
			pos++;
			continue;
		}
		memmove(dst + written, src + pos, (size_t)count);
		written += (size_t)count;
		pos += (size_t)count;
	}
	return written;
}

void utf8_sanitize_inplace(char *s)
{
	if (!s)
		return;
	size_t clean = utf8_sanitize_into(s, s, strlen(s));
	s[clean] = '\0';
}

size_t utf8_copy_sanitized_clamped(char *dst, size_t dst_cap,
				   const char *src, size_t max_bytes)
{
	size_t written = 0;

	if (!dst || !dst_cap)
		return 0;
	if (!src) {
		dst[0] = '\0';
		return 0;
	}
	size_t len = strnlen(src, max_bytes);
	for (size_t pos = 0; pos < len;) {
		utf8proc_int32_t cp;
		utf8proc_ssize_t count = utf8_iterate_bytes(src + pos, len - pos, &cp);

		if (count < 1) {
			pos++;
			continue;
		}
		if ((size_t)count >= dst_cap - written)
			break;
		memmove(dst + written, src + pos, (size_t)count);
		written += (size_t)count;
		pos += (size_t)count;
	}
	dst[written] = '\0';
	return written;
}

int utf8_codepoint_width(unsigned cp)
{
	if (cp > INT32_MAX || !utf8proc_codepoint_valid((utf8proc_int32_t)cp))
		return 0;
	return utf8proc_charwidth((utf8proc_int32_t)cp);
}

size_t utf8_display_width(const char *s)
{
	if (!s)
		return 0;
	size_t n = 0;
	const char *p = s;
	while (*p) {
		utf8_int32_t cp;
		p = utf8codepoint(p, &cp);
		n += (size_t)utf8_codepoint_width((unsigned)cp);
	}
	return n;
}

size_t utf8_display_width_ansi(const char *s)
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
			while (*p && *p != 0x07) {
				if (*p == 0x1B && p[1] == '\\') {
					p += 2;
					goto osc_done;
				}
				p++;
			}
			if (*p == 0x07)
				p++;
osc_done:
			continue;
		}
		if (*p == 0x1B && p[1]) {
			/* Other 2-byte escape (ESC + final). Skip both. */
			p += 2;
			continue;
		}
		utf8_int32_t cp;
		const char *next = utf8codepoint((const char *)p, &cp);
		n += (size_t)utf8_codepoint_width((unsigned)cp);
		p = (const unsigned char *)next;
	}
	return n;
}

const char *utf8_advance_codepoints(const char *s, size_t chars)
{
	if (!s)
		return NULL;
	while (*s && chars > 0) {
		utf8_int32_t cp;
		s = utf8codepoint(s, &cp);
		chars--;
	}
	return s;
}

const char *utf8_advance_display_width(const char *s, size_t cols)
{
	size_t vis = 0;

	if (!s)
		return NULL;
	while (*s && vis < cols) {
		utf8_int32_t cp;
		const char *next = utf8codepoint(s, &cp);
		size_t w = (size_t)utf8_codepoint_width((unsigned)cp);

		if (vis + w > cols)
			break;
		vis += w;
		s = next;
	}
	return s;
}

const char *utf8_suffix_display_width(const char *s, size_t cols)
{
	const char *start;
	const char *p;
	size_t total;
	size_t drop;

	if (!s)
		return NULL;
	total = utf8_display_width(s);
	if (total <= cols)
		return s;
	drop = total - cols;
	start = s;
	p = s;
	while (*p) {
		utf8_int32_t cp;
		const char *next = utf8codepoint(p, &cp);
		size_t w = (size_t)utf8_codepoint_width((unsigned)cp);

		if (drop < w)
			break;
		drop -= w;
		start = next;
		p = next;
	}
	return start;
}

size_t utf8_copy_display_width(char *dst, size_t dst_cap, const char *src,
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
		utf8_int32_t cp;
		const char *next = utf8codepoint(src, &cp);
		size_t cb = (size_t)(next - src);
		size_t w = (size_t)utf8_codepoint_width((unsigned)cp);
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

size_t utf8_copy_sanitized_display_width(char *dst, size_t dst_cap, const char *src,
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

		if (!utf8_decode_codepoint(src, avail, &cp, &cb)) {
			src++;
			avail--;
			continue;
		}
		w = (size_t)utf8_codepoint_width(cp);
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

size_t utf8_copy_ellipsized_display_width(char *dst, size_t dst_cap,
					  const char *src,
					  size_t max_width,
					  int keep_tail)
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
	width = utf8_display_width(src);
	if (width <= max_width)
		return utf8_copy_sanitized_display_width(
			dst, dst_cap, src, max_width);
	if (max_width == 1) {
		(void)utf8_copy_display_width(dst, dst_cap, ellipsis, 1);
		return 1;
	}
	ellipsis_bytes = strlen(ellipsis);
	if (dst_cap <= ellipsis_bytes)
		return utf8_copy_sanitized_display_width(
			dst, dst_cap, src, max_width);
	if (!keep_tail) {
		size_t used_bytes;

		copied = utf8_copy_sanitized_display_width(
			dst, dst_cap, src, max_width - 1);
		used_bytes = strlen(dst);
		if (copied < max_width &&
		    used_bytes + ellipsis_bytes < dst_cap) {
			memcpy(dst + used_bytes, ellipsis, ellipsis_bytes + 1);
			copied++;
		}
		return copied;
	}
	memcpy(dst, ellipsis, ellipsis_bytes);
	dst[ellipsis_bytes] = '\0';
	start = utf8_suffix_display_width(src, max_width - 1);
	copied = utf8_copy_sanitized_display_width(
		dst + ellipsis_bytes, dst_cap - ellipsis_bytes, start,
		max_width - 1);
	return copied + 1;
}

const char *utf8_prev_codepoint(const char *start, const char *p)
{
	if (!start || !p || p <= start)
		return start;
	size_t available = (size_t)(p - start);
	if (available > 4)
		available = 4;
	for (size_t back = 1; back <= available; back++) {
		utf8proc_int32_t cp;
		utf8proc_ssize_t count = utf8_iterate_bytes(p - back, back, &cp);

		if (count == (utf8proc_ssize_t)back)
			return p - back;
	}
	return p - 1;
}

int utf8_is_cjk_sentence_punct(unsigned cp)
{
	return cp == 0x3001 || cp == 0x3002 || cp == 0xFF0C ||
	       cp == 0xFF01 || cp == 0xFF1F || cp == 0xFF1B;
}

int utf8_is_fullwidth_pipe_cp(unsigned cp)
{
	return cp == 0xFF5C;
}

int utf8_is_fullwidth_dash_cp(unsigned cp)
{
	return cp == 0xFF0D;
}

int utf8_is_fullwidth_colon_cp(unsigned cp)
{
	return cp == 0xFF1A;
}

int utf8_is_unicode_space_cp(unsigned cp)
{
	return cp >= 0x80 && utf8_classify_codepoint(cp) == UTF8_CLASS_SEPARATOR;
}

int utf8_is_latin_extended_cp(unsigned cp)
{
	return (cp >= 0x00C0 && cp <= 0x024F) ||
	       (cp >= 0x1E00 && cp <= 0x1EFF);
}

size_t utf8_strip_ansi(char *dst, const char *src, size_t src_len)
{
	size_t j = 0;

	if (!dst || !src)
		return 0;
	for (size_t i = 0; i < src_len; ) {
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
			while (i < src_len && src[i] != '\a') {
				if (src[i] == '\033' && i + 1 < src_len &&
				    src[i + 1] == '\\') {
					i += 2;
					goto osc_done;
				}
				i++;
			}
			if (i < src_len && src[i] == '\a')
				i++;
osc_done:
			continue;
		}
		if (c == 0x1B && i + 1 < src_len && src[i + 1] == '_') {
			i += 2;
			while (i < src_len) {
				if (src[i] == '\033' && i + 1 < src_len &&
				    src[i + 1] == '\\') {
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

char *utf8_strip_ansi_dup(const char *src, size_t src_len,
			  size_t *out_len)
{
	char *out;
	size_t len;

	if (!src)
		return NULL;
	out = malloc(src_len + 1);
	if (!out)
		return NULL;
	len = utf8_strip_ansi(out, src, src_len);
	if (out_len)
		*out_len = len;
	return out;
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

static int terminal_append_visible_control(morph_buf_t *out, unsigned cp)
{
	if (cp == '\b')
		return morph_buf_puts(out, "\\b");
	if (cp == '\a')
		return morph_buf_puts(out, "\\a");
	if (cp <= 0xffu)
		return morph_buf_printf(out, "\\x%02x", cp);
	return morph_buf_printf(out, "\\u%04x", cp);
}

static int terminal_append_text(struct utf8_terminal_sanitizer *sanitizer,
				morph_buf_t *out, const char *text,
				size_t len, unsigned cp)
{
	if (cp == '\n') {
		if (sanitizer->mode == UTF8_TERMINAL_TEXT_SINGLE_LINE)
			return morph_buf_putc(out, ' ');
		return morph_buf_putc(out, '\n');
	}
	if (cp == '\t') {
		if (sanitizer->mode == UTF8_TERMINAL_TEXT_SINGLE_LINE)
			return morph_buf_putc(out, ' ');
		return morph_buf_putc(out, '\t');
	}
	if (cp < 0x20u || (cp >= 0x7fu && cp <= 0x9fu))
		return terminal_append_visible_control(out, cp);
	return morph_buf_append(out, text, len);
}

static int terminal_flush_cr(struct utf8_terminal_sanitizer *sanitizer,
			     morph_buf_t *out)
{
	if (!sanitizer->pending_cr)
		return 0;
	sanitizer->pending_cr = 0;
	if (sanitizer->mode == UTF8_TERMINAL_TEXT_SINGLE_LINE)
		return morph_buf_putc(out, ' ');
	return morph_buf_puts(out, "\\r");
}

static int terminal_process_text_byte(
	struct utf8_terminal_sanitizer *sanitizer, morph_buf_t *out,
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
			return sanitizer->mode ==
				UTF8_TERMINAL_TEXT_SINGLE_LINE ?
				morph_buf_putc(out, ' ') :
				morph_buf_putc(out, '\n');
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
		return terminal_append_text(sanitizer, out,
			(const char *)&src[*index - 1], 1, c);
	}
	expected = (size_t)utf8_prefix_len(src + *index);
	if (expected > 1 && expected <= sizeof(sanitizer->utf8_pending) &&
	    src_len - *index < expected) {
		sanitizer->utf8_pending_len = src_len - *index;
		memcpy(sanitizer->utf8_pending, src + *index,
			sanitizer->utf8_pending_len);
		*index = src_len;
		return 0;
	}
	cp_len = utf8_next_codepoint_len(src + *index, src_len - *index);
	if (cp_len == 0) {
		(*index)++;
		return 0;
	}
	if (!utf8_decode_codepoint(src + *index, cp_len, &cp, &cp_len)) {
		(*index)++;
		return 0;
	}
	rc = terminal_append_text(sanitizer, out, src + *index, cp_len, cp);
	*index += cp_len;
	return rc;
}

static int terminal_process_pending_utf8(
	struct utf8_terminal_sanitizer *sanitizer, morph_buf_t *out,
	const char **src, size_t *src_len, int finish)
{
	while (sanitizer->utf8_pending_len) {
		unsigned char combined[4];
		size_t saved = sanitizer->utf8_pending_len;
		size_t expected = utf8_prefix_len((const char *)sanitizer->utf8_pending);
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
		int rc = utf8_terminal_sanitize_feed(sanitizer, out,
			(const char *)combined + skip, count - skip, finish && *src_len == 0);
		if (rc < 0)
			return rc;
		if (!*src_len)
			return 0;
	}
	return 0;
}

void utf8_terminal_sanitizer_init(struct utf8_terminal_sanitizer *sanitizer,
				  enum utf8_terminal_text_mode mode)
{
	if (!sanitizer)
		return;
	memset(sanitizer, 0, sizeof(*sanitizer));
	sanitizer->mode = mode;
}

void utf8_terminal_sanitizer_reset(struct utf8_terminal_sanitizer *sanitizer)
{
	enum utf8_terminal_text_mode mode;

	if (!sanitizer)
		return;
	mode = sanitizer->mode;
	memset(sanitizer, 0, sizeof(*sanitizer));
	sanitizer->mode = mode;
}

int utf8_terminal_sanitize_feed(struct utf8_terminal_sanitizer *sanitizer,
				morph_buf_t *out, const char *src,
				size_t src_len, int finish)
{
	size_t i = 0;
	int rc;

	if (!sanitizer || !out || (!src && src_len > 0))
		return -EINVAL;
	if (!src)
		src = "";
	rc = terminal_process_pending_utf8(sanitizer, out, &src, &src_len,
		finish);
	if (rc != 0)
		return rc;
	while (i < src_len) {
		unsigned char c = (unsigned char)src[i];

		switch (sanitizer->state) {
		case TERMINAL_SANITIZE_TEXT:
			if (sanitizer->pending_cr &&
			    (c == 0x1bu || c == 0x90u || c == 0x98u ||
			     c == 0x9bu || c == 0x9du || c == 0x9eu ||
			     c == 0x9fu)) {
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
			} else if (c == 0x90u || c == 0x98u ||
				   c == 0x9eu || c == 0x9fu) {
				sanitizer->state = TERMINAL_SANITIZE_STRING;
				i++;
			} else {
				rc = terminal_process_text_byte(sanitizer, out,
					src, src_len, &i);
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
			utf8_terminal_sanitizer_reset(sanitizer);
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
		utf8_terminal_sanitizer_reset(sanitizer);
		if (rc != 0)
			return rc;
	}
	return 0;
}

char *utf8_terminal_sanitize_dup(const char *src, size_t src_len,
			 enum utf8_terminal_text_mode mode,
			 size_t *out_len)
{
	struct utf8_terminal_sanitizer sanitizer;
	morph_buf_t out;
	char *result;
	int rc;

	if (!src)
		return NULL;
	if (src_len > SIZE_MAX - 16)
		return NULL;
	rc = morph_buf_init(&out, src_len + 16);
	if (rc != 0)
		return NULL;
	utf8_terminal_sanitizer_init(&sanitizer, mode);
	rc = utf8_terminal_sanitize_feed(&sanitizer, &out, src, src_len, 1);
	if (rc != 0) {
		morph_buf_cleanup(&out);
		return NULL;
	}
	if (out_len)
		*out_len = out.len;
	result = morph_buf_detach(&out);
	return result;
}

int utf8_is_cjk_cp(unsigned cp)
{
	if (cp >= 0x4E00 && cp <= 0x9FFF) return 1;
	if (cp >= 0x3400 && cp <= 0x4DBF) return 1;
	if (cp >= 0xF900 && cp <= 0xFAFF) return 1;
	if (cp >= 0x20000 && cp <= 0x2EBEF) return 1;
	if (cp >= 0x30000 && cp <= 0x3134F) return 1;
	if (cp >= 0x2E80 && cp <= 0x2EFF) return 1;
	if (cp >= 0x2F00 && cp <= 0x2FDF) return 1;
	if (cp >= 0x3100 && cp <= 0x312F) return 1;
	if (cp >= 0x3040 && cp <= 0x309F) return 1;
	if (cp >= 0x30A0 && cp <= 0x30FF) return 1;
	if (cp >= 0xAC00 && cp <= 0xD7AF) return 1;
	if (cp >= 0x1100 && cp <= 0x11FF) return 1;
	if (cp >= 0x3130 && cp <= 0x318F) return 1;
	if (cp >= 0xFF01 && cp <= 0xFFEF) return 1;
	return 0;
}

int utf8_is_hiragana_cp(unsigned cp)
{
	return cp >= 0x3040 && cp <= 0x309F;
}

int utf8_is_katakana_cp(unsigned cp)
{
	return cp >= 0x30A0 && cp <= 0x30FF;
}

int utf8_is_hangul_cp(unsigned cp)
{
	return (cp >= 0xAC00 && cp <= 0xD7AF) ||
	       (cp >= 0x1100 && cp <= 0x11FF) ||
	       (cp >= 0x3130 && cp <= 0x318F);
}
