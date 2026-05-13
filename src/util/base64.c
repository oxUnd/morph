#include "base64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define B64_ALPHABET "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

char *base64_encode(const unsigned char *data, size_t data_len)
{
	if (!data && data_len > 0)
		return NULL;

	size_t b64_len = (data_len + 2) / 3 * 4;
	char *b64 = malloc(b64_len + 1);
	if (!b64)
		return NULL;

	size_t j = 0;
	for (size_t i = 0; i < data_len; i += 3) {
		unsigned int v = (unsigned int)data[i] << 16;
		if (i + 1 < data_len)
			v |= (unsigned int)data[i + 1] << 8;
		if (i + 2 < data_len)
			v |= (unsigned int)data[i + 2];
		b64[j++] = B64_ALPHABET[(v >> 18) & 0x3F];
		b64[j++] = B64_ALPHABET[(v >> 12) & 0x3F];
		b64[j++] = (i + 1 < data_len) ? B64_ALPHABET[(v >> 6) & 0x3F] : '=';
		b64[j++] = (i + 2 < data_len) ? B64_ALPHABET[v & 0x3F] : '=';
	}
	b64[j] = '\0';
	return b64;
}

char *base64_encode_file(const char *path)
{
	if (!path)
		return NULL;

	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;

	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0) {
		fclose(f);
		return NULL;
	}

	unsigned char *data = malloc((size_t)sz);
	if (!data) {
		fclose(f);
		return NULL;
	}

	size_t rd = fread(data, 1, (size_t)sz, f);
	fclose(f);

	if (rd == 0) {
		free(data);
		return NULL;
	}

	char *b64 = base64_encode(data, rd);
	free(data);
	return b64;
}
