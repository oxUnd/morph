#include "str.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

char *morph_strndup(const char *data, size_t len)
{
	char *copy;

	if (!data || len == SIZE_MAX)
		return NULL;
	copy = malloc(len + 1);
	if (!copy)
		return NULL;
	memcpy(copy, data, len);
	copy[len] = '\0';
	return copy;
}

char *morph_strdup(const char *text)
{
	return morph_strndup(text, text ? strlen(text) : 0);
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

char *morph_str_cstr_copy(morph_str_t text)
{
	return morph_strndup(text.data, text.len);
}

const char *morph_str_chr_borrow(morph_str_t s, char c)
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

const char *morph_str_rchr_borrow(morph_str_t s, char c)
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

morph_str_t morph_str_trim_borrow(morph_str_t s)
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
