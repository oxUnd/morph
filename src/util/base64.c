#include "base64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define B64_ALPHABET "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

static int base64_value(unsigned char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

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

unsigned char *base64_decode(const char *text, size_t *out_len)
{
	unsigned char *out = NULL;
	size_t len;
	size_t padding = 0;
	size_t decoded_len;
	size_t j = 0;

	if (out_len)
		*out_len = 0;
	if (!text)
		return NULL;
	len = strlen(text);
	if (len == 0 || len % 4 != 0)
		return NULL;
	if (text[len - 1] == '=')
		padding++;
	if (text[len - 2] == '=')
		padding++;
	decoded_len = len / 4 * 3 - padding;
	out = malloc(decoded_len > 0 ? decoded_len : 1);
	if (!out)
		return NULL;

	for (size_t i = 0; i < len; i += 4) {
		int a = base64_value((unsigned char)text[i]);
		int b = base64_value((unsigned char)text[i + 1]);
		int c = text[i + 2] == '=' ? 0 :
			base64_value((unsigned char)text[i + 2]);
		int d = text[i + 3] == '=' ? 0 :
			base64_value((unsigned char)text[i + 3]);
		int final = i + 4 == len;

		if (a < 0 || b < 0 || c < 0 || d < 0 ||
		    (!final && (text[i + 2] == '=' || text[i + 3] == '=')) ||
		    (text[i + 2] == '=' && text[i + 3] != '=')) {
			free(out);
			return NULL;
		}
		if (j < decoded_len)
			out[j++] = (unsigned char)((a << 2) | (b >> 4));
		if (j < decoded_len)
			out[j++] = (unsigned char)((b << 4) | (c >> 2));
		if (j < decoded_len)
			out[j++] = (unsigned char)((c << 6) | d);
	}
	if (out_len)
		*out_len = decoded_len;
	return out;
}
