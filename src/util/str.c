#include "str.h"
#include "arena.h"
#include <stdint.h>
#include <string.h>
#include <ctype.h>

morph_str_t morph_strndup(struct arena *a, const char *data, size_t len)
{
	morph_str_t s = MORPH_STR_NULL;
	char *dst;

	if (!data) {
		s.len = 0;
		s.data = NULL;
		return s;
	}
	if (!a || len == SIZE_MAX)
		return s;
	dst = arena_alloc(a, len + 1);
	if (!dst) {
		s.len = 0;
		s.data = NULL;
		return s;
	}
	memcpy(dst, data, len);
	dst[len] = '\0';
	s.len = len;
	s.data = dst;
	return s;
}

morph_str_t morph_strdup(struct arena *a, const char *s)
{
	return morph_strndup(a, s, s ? strlen(s) : 0);
}

int morph_strcmp(morph_str_t a, morph_str_t b)
{
	int cmp;

	if (a.len == 0 && b.len == 0)
		return 0;
	if (a.len != b.len)
		return a.len < b.len ? -1 : 1;
	if (!a.data || !b.data)
		return a.data == b.data ? 0 : (!a.data ? -1 : 1);
	cmp = memcmp(a.data, b.data, a.len);
	return cmp;
}

int morph_strcasecmp(morph_str_t a, morph_str_t b)
{
	size_t i;

	if (a.len == 0 && b.len == 0)
		return 0;
	if (a.len != b.len)
		return a.len < b.len ? -1 : 1;
	if (!a.data || !b.data)
		return a.data == b.data ? 0 : (!a.data ? -1 : 1);
	for (i = 0; i < a.len; i++) {
		int ca = tolower((unsigned char)a.data[i]);
		int cb = tolower((unsigned char)b.data[i]);
		if (ca != cb)
			return ca < cb ? -1 : 1;
	}
	return 0;
}

int morph_strncmp(morph_str_t a, const char *b, size_t n)
{
	size_t cmplen = a.len < n ? a.len : n;
	int cmp;

	if (cmplen == 0)
		return 0;
	if (!a.data || !b)
		return a.data == b ? 0 : (!a.data ? -1 : 1);
	cmp = memcmp(a.data, b, cmplen);
	if (cmp != 0)
		return cmp;
	if (a.len >= n)
		return 0;
	return -1;
}

const char *morph_str_to_c(struct arena *a, morph_str_t s)
{
	char *dst;

	if (!s.data)
		return NULL;
	if (!a || s.len == SIZE_MAX)
		return NULL;
	dst = arena_alloc(a, s.len + 1);
	if (!dst)
		return NULL;
	memcpy(dst, s.data, s.len);
	dst[s.len] = '\0';
	return dst;
}

const char *morph_str_chr(morph_str_t s, char c)
{
	size_t i;

	if (!s.data)
		return NULL;
	for (i = 0; i < s.len; i++) {
		if (s.data[i] == c)
			return s.data + i;
	}
	return NULL;
}

const char *morph_str_rchr(morph_str_t s, char c)
{
	if (!s.data || s.len == 0)
		return NULL;
	{
		const char *p = s.data + s.len;
		while (p > s.data) {
			p--;
			if (*p == c)
				return p;
		}
	}
	return NULL;
}

morph_str_t morph_str_trim(morph_str_t s)
{
	size_t start = 0;
	size_t end;

	if (!s.data)
		return s;
	while (start < s.len &&
	       ((unsigned char)s.data[start] == ' ' ||
	        (unsigned char)s.data[start] == '\t' ||
	        (unsigned char)s.data[start] == '\n' ||
	        (unsigned char)s.data[start] == '\r'))
		start++;
	if (start >= s.len) {
		morph_str_t empty = MORPH_STR_EMPTY;
		return empty;
	}
	end = s.len;
	while (end > start &&
	       ((unsigned char)s.data[end - 1] == ' ' ||
	        (unsigned char)s.data[end - 1] == '\t' ||
	        (unsigned char)s.data[end - 1] == '\n' ||
	        (unsigned char)s.data[end - 1] == '\r'))
		end--;
	{
		morph_str_t r;
		r.len = end - start;
		r.data = s.data + start;
		return r;
	}
}
