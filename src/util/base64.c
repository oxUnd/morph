#include "base64.h"
#include "error.h"
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
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

static size_t base64_encode_into(const unsigned char *data, size_t data_len,
				 char *b64)
{
	size_t j = 0;

	for (size_t i = 0; i < data_len; i += 3) {
		unsigned int v = (unsigned int)data[i] << 16;
		if (i + 1 < data_len)
			v |= (unsigned int)data[i + 1] << 8;
		if (i + 2 < data_len)
			v |= (unsigned int)data[i + 2];
		b64[j++] = B64_ALPHABET[(v >> 18) & 0x3F];
		b64[j++] = B64_ALPHABET[(v >> 12) & 0x3F];
		b64[j++] = i + 1 < data_len ? B64_ALPHABET[(v >> 6) & 0x3F] : '=';
		b64[j++] = i + 2 < data_len ? B64_ALPHABET[v & 0x3F] : '=';
	}
	b64[j] = '\0';
	return j;
}

char *base64_encode(const unsigned char *data, size_t data_len)
{
	char *b64;

	if ((!data && data_len > 0) || data_len > (SIZE_MAX / 4) * 3 - 2)
		return NULL;
	b64 = malloc((data_len + 2) / 3 * 4 + 1);
	if (b64)
		(void)base64_encode_into(data, data_len, b64);
	return b64;
}

int base64_encode_file_prefixed(const char *path, const char *prefix,
			       size_t max_bytes, char **out)
{
	unsigned char chunk[BUFSIZ - BUFSIZ % 3];
	struct stat st;
	FILE *file;
	char *encoded;
	size_t length, prefix_len, remaining, offset, capacity;
	int rc = 0;

	if (!path || !prefix || !out)
		MORPH_RETURN(-EINVAL);
	*out = NULL;
	file = fopen(path, "rb");
	if (!file)
		MORPH_RETURN(-errno);
	if (fstat(fileno(file), &st) != 0) {
		rc = -errno;
		fclose(file);
		MORPH_RETURN(rc);
	}
	if (!S_ISREG(st.st_mode) || st.st_size <= 0) {
		fclose(file);
		MORPH_RETURN(-EINVAL);
	}
	if ((uintmax_t)st.st_size > max_bytes ||
	    (uintmax_t)st.st_size > (SIZE_MAX / 4) * 3 - 2) {
		fclose(file);
		MORPH_RETURN(-EFBIG);
	}
	length = (size_t)st.st_size;
	prefix_len = strlen(prefix);
	capacity = (length + 2) / 3 * 4 + 1;
	if (prefix_len > SIZE_MAX - capacity) {
		fclose(file);
		MORPH_RETURN(-EOVERFLOW);
	}
	encoded = malloc(prefix_len + capacity);
	if (!encoded) {
		fclose(file);
		MORPH_RETURN(-ENOMEM);
	}
	memcpy(encoded, prefix, prefix_len);
	offset = prefix_len;
	remaining = length;
	while (remaining > 0) {
		size_t wanted = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
		size_t got = fread(chunk, 1, wanted, file);

		if (got != wanted) {
			rc = ferror(file) && errno ? -errno : -EIO;
			break;
		}
		offset += base64_encode_into(chunk, got, encoded + offset);
		remaining -= got;
	}
	if (rc == 0 && fgetc(file) != EOF)
		rc = -EFBIG;
	if (rc == 0 && ferror(file))
		rc = errno ? -errno : -EIO;
	fclose(file);
	if (rc != 0) {
		free(encoded);
		MORPH_RETURN(rc);
	}
	*out = encoded;
	return 0;
}

char *base64_encode_file(const char *path)
{
	char *encoded = NULL;

	if (base64_encode_file_prefixed(path, "", MORPH_MEDIA_MAX_FILE_BYTES,
				       &encoded) != 0)
		return NULL;
	return encoded;
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
