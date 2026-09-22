#include "util/buf.h"
#include "util/error.h"
#include "util/image_util.h"
#include "stb_image.h"
#include "stb_image_write.h"

#include <curl/curl.h>
#include <webp/decode.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CLI_IMAGE_MAX_BYTES (64u * 1024u * 1024u)

static size_t image_receive(char *data, size_t size, size_t count, void *user)
{
	morph_buf_t *buffer = user;
	size_t bytes;

	if (size && count > CLI_IMAGE_MAX_BYTES / size)
		return 0;
	bytes = size * count;
	if (bytes > CLI_IMAGE_MAX_BYTES - buffer->len)
		return 0;
	return morph_buf_append(buffer, data, bytes) == 0 ? bytes : 0;
}

static int image_fetch(const char *url, morph_buf_t *buffer)
{
	CURL *curl = curl_easy_init();
	CURLcode result;
	long status = 0;

	if (!curl)
		MORPH_RETURN(-ENOMEM);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, image_receive);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer);
	result = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	if (result != CURLE_OK)
		MORPH_RETURN(MORPH_ERR_NETWORK);
	if (status < 200 || status >= 300)
		MORPH_RETURN(MORPH_ERR_API);
	return 0;
}

static int image_read(const char *url, morph_buf_t *buffer)
{
	const char *path = strncmp(url, "file://", 7) == 0 ? url + 7 : url;
	char bytes[BUFSIZ];
	size_t count;
	FILE *file;
	struct stat info;
	int rc = 0;

	if (strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0)
		return image_fetch(url, buffer);
	if (stat(path, &info) != 0)
		MORPH_RETURN(-errno);
	if (!S_ISREG(info.st_mode) || info.st_size > CLI_IMAGE_MAX_BYTES)
		MORPH_RETURN(-EFBIG);
	file = fopen(path, "rb");
	if (!file)
		MORPH_RETURN(-errno);
	while ((count = fread(bytes, 1, sizeof(bytes), file)) > 0) {
		if (image_receive(bytes, 1, count, buffer) != count) {
			MORPH_SET_ERR(rc, -EFBIG);
			break;
		}
	}
	if (ferror(file))
		MORPH_SET_ERR(rc, -EIO);
	fclose(file);
	return rc;
}

static unsigned char *image_decode(const morph_buf_t *buffer, int *w, int *h)
{
	const unsigned char *data = (const unsigned char *)morph_buf_cstr(buffer);
	int channels;
	size_t bytes;
	unsigned char *pixels;
	int webp = WebPGetInfo(data, buffer->len, w, h);

	if (!webp && !stbi_info_from_memory(data, (int)buffer->len, w, h, &channels))
		return NULL;
	if (image_pixel_bytes(*w, *h, 4, &bytes) != 0)
		return NULL;
	if (!webp)
		return stbi_load_from_memory(data, (int)buffer->len, w, h, &channels, 4);
	pixels = malloc(bytes);
	if (!pixels)
		return NULL;
	if (!WebPDecodeRGBAInto(data, buffer->len, pixels, bytes, *w * 4)) {
		free(pixels);
		return NULL;
	}
	return pixels;
}

int cli_markdown_load_image(const char *url, char **path, void *user)
{
	morph_buf_t buffer;
	unsigned char *pixels;
	char temporary[] = "/tmp/morph-markdown-image-XXXXXX";
	int w, h, fd, rc;

	(void)user;
	if (!url || !path)
		MORPH_RETURN(-EINVAL);
	*path = NULL;
	rc = morph_buf_init(&buffer, BUFSIZ);
	if (rc != 0)
		return rc;
	rc = image_read(url, &buffer);
	pixels = rc == 0 ? image_decode(&buffer, &w, &h) : NULL;
	morph_buf_cleanup(&buffer);
	if (!pixels)
		MORPH_RETURN(rc != 0 ? rc : MORPH_ERR_FORMAT);
	fd = mkstemp(temporary);
	if (fd < 0) {
		rc = -errno;
		free(pixels);
		MORPH_RETURN(rc);
	}
	close(fd);
	rc = stbi_write_png(temporary, w, h, 4, pixels, w * 4);
	free(pixels);
	if (rc)
		*path = strdup(temporary);
	if (!*path) {
		unlink(temporary);
		MORPH_RETURN(rc ? -ENOMEM : -EIO);
	}
	return 0;
}

void cli_markdown_release_image(char *path, void *user)
{
	(void)user;
	unlink(path);
	free(path);
}
