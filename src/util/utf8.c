#define _GNU_SOURCE

#include "utf8.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

size_t utf8_clamp_bytes(const char *s, size_t max_bytes)
{
	if (!s)
		return 0;
	size_t len = strlen(s);
	if (len <= max_bytes)
		return len;
	size_t pos = max_bytes;
	while (pos > 0 && utf8nvalid(s, pos) != NULL)
		pos--;
	return pos;
}

size_t utf8_next_codepoint_len(const char *s, size_t avail)
{
	size_t cp_len;

	if (!s || avail == 0 || *s == '\0')
		return 0;
	cp_len = (size_t)utf8codepointcalcsize(s);
	if (cp_len == 0 || cp_len > avail)
		return 1;
	if (utf8nvalid(s, cp_len) != NULL)
		return 1;
	return cp_len;
}

int utf8_decode_codepoint(const char *s, size_t avail, unsigned *cp,
			  size_t *cp_len)
{
	utf8_int32_t raw;
	size_t len;

	if (!s || avail == 0 || !cp || !cp_len)
		return 0;
	len = utf8_next_codepoint_len(s, avail);
	if (len == 0)
		return 0;
	if (len == 1 && (unsigned char)*s >= 0x80)
		return 0;
	(void)utf8codepoint(s, &raw);
	*cp = (unsigned)raw;
	*cp_len = len;
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
	if (!dst || !src)
		return 0;
	size_t i = 0;
	size_t j = 0;
	while (i < src_len) {
		unsigned char c = (unsigned char)src[i];
		if (c == 0) { i++; continue; }
		if (c < 0x80) {
			dst[j++] = (char)c;
			i++;
			continue;
		}
		size_t cp_len = (size_t)utf8codepointcalcsize(src + i);
		if (cp_len == 0 || i + cp_len > src_len) { i++; continue; }
		const char *v = utf8nvalid(src + i, cp_len);
		if (v != NULL) { i++; continue; }
		for (size_t k = 0; k < cp_len; k++)
			dst[j++] = src[i + k];
		i += cp_len;
	}
	return j;
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
	size_t src_len;
	size_t safe;
	size_t clean;

	if (!dst || dst_cap == 0)
		return 0;
	if (!src) {
		dst[0] = '\0';
		return 0;
	}

	src_len = strlen(src);
	if (src_len > max_bytes)
		src_len = max_bytes;
	if (src_len >= dst_cap)
		src_len = dst_cap - 1;
	safe = utf8_clamp_bytes(src, src_len);
	clean = utf8_sanitize_into(dst, src, safe);
	dst[clean] = '\0';
	return clean;
}

int utf8_codepoint_width(unsigned cp)
{
	if (cp == 0)
		return 0;
	if (cp < 0x20)
		return 0;
	{
		int w = wcwidth((wchar_t)cp);
		if (w > 0)
			return w;
		if (w == 0)
			return 0;
	}
	if (cp >= 0x0300 && cp <= 0x036F) return 0;
	if (cp >= 0x0483 && cp <= 0x0489) return 0;
	if (cp >= 0x1AB0 && cp <= 0x1AFF) return 0;
	if (cp >= 0x1DC0 && cp <= 0x1DFF) return 0;
	if (cp >= 0x20D0 && cp <= 0x20FF) return 0;
	if (cp >= 0xFE20 && cp <= 0xFE2F) return 0;
	if (cp == 0x00AD) return 0;
	if (cp == 0x034F) return 0;
	if (cp == 0x200B) return 0;
	if (cp == 0x200C) return 0;
	if (cp == 0x200D) return 0;
	if (cp == 0x2060) return 0;
	if (cp >= 0xFE00 && cp <= 0xFE0F) return 0;
	if (cp >= 0xE0100 && cp <= 0xE01EF) return 0;
	if ((cp >= 0x1100 && cp <= 0x115F) ||
	    (cp >= 0x2329 && cp <= 0x232A) ||
	    (cp >= 0x2E80 && cp <= 0x303E) ||
	    (cp >= 0x3040 && cp <= 0x334F) ||
	    (cp >= 0x3400 && cp <= 0x4DBF) ||
	    (cp >= 0x4E00 && cp <= 0x9FFF) ||
	    (cp >= 0xA000 && cp <= 0xA4CF) ||
	    (cp >= 0xA960 && cp <= 0xA97C) ||
	    (cp >= 0xAC00 && cp <= 0xD7A3) ||
	    (cp >= 0xD7B0 && cp <= 0xD7C6) ||
	    (cp >= 0xF900 && cp <= 0xFAFF) ||
	    (cp >= 0xFE10 && cp <= 0xFE19) ||
	    (cp >= 0xFE30 && cp <= 0xFE6F) ||
	    (cp >= 0xFF01 && cp <= 0xFF60) ||
	    (cp >= 0xFFE0 && cp <= 0xFFE6) ||
	    (cp >= 0x20000 && cp <= 0x2FFFD) ||
	    (cp >= 0x30000 && cp <= 0x3FFFD))
		return 2;
	if ((cp >= 0x1F600 && cp <= 0x1F64F) ||
	    (cp >= 0x1F300 && cp <= 0x1F5FF) ||
	    (cp >= 0x1F680 && cp <= 0x1F6FF) ||
	    (cp >= 0x1F900 && cp <= 0x1F9FF) ||
	    (cp >= 0x1FA00 && cp <= 0x1FA6F) ||
	    (cp >= 0x1FA70 && cp <= 0x1FAFF) ||
	    (cp >= 0x2600 && cp <= 0x26FF) ||
	    (cp >= 0x2700 && cp <= 0x27BF) ||
	    (cp >= 0x1F000 && cp <= 0x1F02F) ||
	    (cp >= 0x1F0A0 && cp <= 0x1F0FF))
		return 2;
	return 1;
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
	while (*s && chars > 0) {
		s += (size_t)utf8codepointcalcsize(s);
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
		(void)utf8codepoint(src, &cp);
		size_t cb = (size_t)utf8codepointcalcsize(src);
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

const char *utf8_prev_codepoint(const char *start, const char *p)
{
	if (!start || !p || p <= start)
		return start;
	p--;
	while (p > start && ((unsigned char)*p & 0xC0) == 0x80)
		p--;
	return p;
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
	return cp == 0x00A0 || cp == 0x1680 || cp == 0x3000 ||
	       (cp >= 0x2000 && cp <= 0x200A) ||
	       cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
	       cp == 0x205F;
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
