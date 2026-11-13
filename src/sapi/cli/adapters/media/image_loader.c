#include "sapi/cli/adapters/media/image_loader.h"
#include "stb_image.h"
#include "http/client.h"
#include "nb/buf.h"
#include "nb/file.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "media/image.h"
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

#define CLI_IMAGE_LOADER_IMAGE_MAX_BYTES (64u * 1024u * 1024u)

static size_t cli_image_loader_receive(char *data, size_t size, size_t count, void *user)
{
	nb_buf *buffer = user;
	size_t bytes;

	if (size && count > CLI_IMAGE_LOADER_IMAGE_MAX_BYTES / size)
		return 0;
	bytes = size * count;
	if (bytes > CLI_IMAGE_LOADER_IMAGE_MAX_BYTES - buffer->len)
		return 0;
	return (nb_buf_append(buffer, data, bytes) ? 0 : NB_ERR_NOMEM) == 0 ? bytes : 0;
}

static int cli_image_loader_fetch(const char *url, nb_buf *buffer)
{
	CURL *curl = curl_easy_init();
	CURLcode result;
	long status = 0;

	if (!curl)
		return NB_ERR_NOMEM;
	http_apply_ca_env(curl);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cli_image_loader_receive);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer);
	result = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	if (result != CURLE_OK)
		return MORPH_ERR_NETWORK;
	if (status < 200 || status >= 300)
		return MORPH_ERR_API;
	return 0;
}

static int cli_image_loader_read(const char *url, nb_buf *buffer)
{
	const char *path = strncmp(url, "file://", 7) == 0 ? url + 7 : url;
	char bytes[BUFSIZ];
	size_t count;
	FILE *file;
	struct stat info;
	int rc = 0;

	if (strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0)
		return cli_image_loader_fetch(url, buffer);
	if (stat(path, &info) != 0)
		return nb_status_from_errno(errno, NULL);
	if (!S_ISREG(info.st_mode) || info.st_size > CLI_IMAGE_LOADER_IMAGE_MAX_BYTES)
		return NB_ERR_TOO_BIG;
	file = fopen(path, "rb");
	if (!file)
		return nb_status_from_errno(errno, NULL);
	while ((count = fread(bytes, 1, sizeof(bytes), file)) > 0) {
		if (cli_image_loader_receive(bytes, 1, count, buffer) != count) {
			rc = (NB_ERR_TOO_BIG);
			break;
		}
	}
	if (ferror(file))
		rc = (NB_ERR_IO);
	fclose(file);
	return rc;
}

static unsigned char *cli_image_loader_decode(const nb_buf *buffer, int *w, int *h)
{
	const unsigned char *data = (const unsigned char *)nb_buf_cstr(buffer);
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

int cli_image_loader_load(const char *url, char **path, void *user)
{
	nb_buf buffer NB_CLEANUP(nb_buf_cleanup) = {0};
	unsigned char *pixels;
	nb_buf temporary_path NB_CLEANUP(nb_buf_cleanup) = {0};
	int w, h, rc;

	(void)user;
	if (!url || !path)
		return NB_ERR_INVAL;
	if (*path)
		return NB_ERR_EXISTS;
	rc = (nb_buf_init(&buffer), nb_buf_reserve(&buffer, BUFSIZ) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = cli_image_loader_read(url, &buffer);
	pixels = rc == 0 ? cli_image_loader_decode(&buffer, &w, &h) : NULL;
	nb_buf_cleanup(&buffer);
	if (!pixels)
		return rc != 0 ? rc : NB_ERR_FORMAT;
	rc = nb_file_temp_in(&temporary_path, "/tmp", NULL, NULL);
	if (rc != NB_OK) {
		free(pixels);
		return rc;
	}
	const char *temporary = nb_buf_cstr(&temporary_path);
	rc = stbi_write_png(temporary, w, h, 4, pixels, w * 4);
	free(pixels);
	if (rc)
		*path = strdup(temporary);
	if (!*path) {
		unlink(temporary);
		return rc ? NB_ERR_NOMEM : NB_ERR_IO;
	}
	return 0;
}

void cli_image_loader_release(char *path, void *user)
{
	(void)user;
	unlink(path);
	free(path);
}
