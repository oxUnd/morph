#define _GNU_SOURCE

#include "utf8.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <locale.h>

size_t utf8_safe_len(const char *s, size_t max_bytes)
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
		size_t cut = utf8_safe_len(src, max_bytes);
		char *dup = malloc(cut + 1);
		if (!dup)
			return NULL;
		memcpy(dup, src, cut);
		dup[cut] = '\0';
		return dup;
	}
	size_t cut = utf8_safe_len(src, max_bytes - marker_len);
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

int utf8_cp_width(unsigned cp)
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

size_t utf8_visible_len(const char *s)
{
	if (!s)
		return 0;
	size_t n = 0;
	const char *p = s;
	while (*p) {
		utf8_int32_t cp;
		p = utf8codepoint(p, &cp);
		n += (size_t)utf8_cp_width((unsigned)cp);
	}
	return n;
}

size_t utf8_visible_len_ansi(const char *s)
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
		n += (size_t)utf8_cp_width((unsigned)cp);
		p = (const unsigned char *)next;
	}
	return n;
}

const char *utf8_skip_forward(const char *s, size_t chars)
{
	while (*s && chars > 0) {
		s += (size_t)utf8codepointcalcsize(s);
		chars--;
	}
	return s;
}

size_t utf8_copy_vis(char *dst, size_t dst_cap, const char *src, size_t max_vis)
{
	size_t written = 0;
	size_t vis = 0;
	if (!dst || dst_cap == 0)
		return 0;
	while (*src) {
		utf8_int32_t cp;
		(void)utf8codepoint(src, &cp);
		size_t cb = (size_t)utf8codepointcalcsize(src);
		size_t w = (size_t)utf8_cp_width((unsigned)cp);
		if (vis + w > max_vis)
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

size_t utf8_truncate(const char *s, size_t max_bytes)
{
	if (!s) return 0;
	size_t len = strlen(s);
	if (len <= max_bytes) return len;
	size_t pos = max_bytes;
	while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80)
		pos--;
	return pos;
}

int utf8_display_width(const char *s)
{
	if (!s) return 0;
	return (int)utf8_visible_len(s);
}
