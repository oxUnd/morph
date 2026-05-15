#include "utf8.h"

#include <stdlib.h>
#include <string.h>

size_t utf8_safe_len(const char *s, size_t max_bytes)
{
	if (!s)
		return 0;
	size_t len = strlen(s);
	if (len <= max_bytes)
		return len;
	size_t pos = max_bytes;
	while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80)
		pos--;
	return pos;
}

char *utf8_dup_clamped(const char *src, size_t max_bytes)
{
	if (!src)
		return NULL;
	size_t len = strlen(src);
	if (len <= max_bytes) {
		char *dup = malloc(len + 1);
		if (!dup)
			return NULL;
		memcpy(dup, src, len + 1);
		return dup;
	}
	static const char marker[] = "\xE2\x80\xA6(truncated)";	/* "…(truncated)" */
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
		size_t need;
		if      ((c & 0xE0) == 0xC0) need = 2;
		else if ((c & 0xF0) == 0xE0) need = 3;
		else if ((c & 0xF8) == 0xF0) need = 4;
		else { i++; continue; }
		if (i + need > src_len) break;
		int ok = 1;
		for (size_t k = 1; k < need; k++) {
			if (((unsigned char)src[i + k] & 0xC0) != 0x80) {
				ok = 0;
				break;
			}
		}
		if (!ok) { i++; continue; }
		for (size_t k = 0; k < need; k++)
			dst[j++] = src[i + k];
		i += need;
	}
	return j;
}
