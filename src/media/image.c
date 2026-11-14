#include "image.h"
#include "nb/cleanup.h"
#include "nb/encoding.h"
#include "nb/buf.h"
#include "nb/file.h"
#include "errors.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#define IMAGE_GEN_MIN_PIXELS (2560LL * 1440LL)
#define IMAGE_GEN_MAX_PIXELS (4096LL * 4096LL)
#define IMAGE_JPEG_QUALITY 88

int image_pixel_bytes(int width, int height, int channels, size_t *bytes)
{
	size_t pixels;

	if (!bytes || width <= 0 || height <= 0 || channels < 1 || channels > 4)
		return NB_ERR_INVAL;
	if (width > MORPH_IMAGE_MAX_DIMENSION || height > MORPH_IMAGE_MAX_DIMENSION ||
	    (size_t)width > MORPH_IMAGE_MAX_PIXELS / (size_t)height)
		return NB_ERR_TOO_BIG;
	pixels = (size_t)width * (size_t)height;
	*bytes = pixels * (size_t)channels;
	return 0;
}

int image_validate_file(const char *path)
{
	struct stat st;
	int w, h, ch;
	size_t bytes;

	if (!path)
		return NB_ERR_INVAL;
	if (stat(path, &st) != 0)
		return nb_status_from_errno(errno, NULL);
	if (!S_ISREG(st.st_mode) || st.st_size > MORPH_MEDIA_MAX_FILE_BYTES)
		return NB_ERR_TOO_BIG;
	if (!stbi_info(path, &w, &h, &ch))
		return NB_ERR_FORMAT;
	return image_pixel_bytes(w, h, 4, &bytes);
}

unsigned char *image_load_bounded_copy(const char *path, int *width, int *height, int *channels,
				       int desired_channels)
{
	if (image_validate_file(path) != 0)
		return NULL;
	return stbi_load(path, width, height, channels, desired_channels);
}

enum image_encode_format {
	IMAGE_ENCODE_PNG,
	IMAGE_ENCODE_JPEG,
	IMAGE_ENCODE_OTHER,
};

struct image_encode_sink {
	nb_buf buf;
	int rc;
};

static enum image_encode_format image_encode_detect_format(const char *path)
{
	unsigned char header[8] = {0};
	FILE *file;
	size_t read_len;

	file = fopen(path, "rb");
	if (!file)
		return IMAGE_ENCODE_OTHER;
	read_len = fread(header, 1, sizeof(header), file);
	fclose(file);
	if (read_len >= 8 &&
	    memcmp(header, "\x89PNG\r\n\x1a\n", sizeof(header)) == 0)
		return IMAGE_ENCODE_PNG;
	if (read_len >= 3 && header[0] == 0xff && header[1] == 0xd8 &&
	    header[2] == 0xff)
		return IMAGE_ENCODE_JPEG;
	return IMAGE_ENCODE_OTHER;
}

static void image_encode_sink_write(void *context, void *data, int size)
{
	struct image_encode_sink *sink = context;

	if (!sink || sink->rc != 0 || !data || size <= 0)
		return;
	sink->rc = (nb_buf_append(&sink->buf, (const char *)data, (size_t)size) ? 0 : NB_ERR_NOMEM);
}

static const char *image_out_ext(const char *path)
{
	const char *e = strrchr(path, '.');

	if (!e)
		return "png";
	if (strcasecmp(e, ".jpg") == 0 || strcasecmp(e, ".jpeg") == 0)
		return "jpg";
	if (strcasecmp(e, ".bmp") == 0)
		return "bmp";
	if (strcasecmp(e, ".tga") == 0)
		return "tga";
	return "png";
}

static int image_write_by_ext(struct image_encode_sink *sink, const char *ext, int w, int h, int ch,
			      const unsigned char *data)
{
	if (strcmp(ext, "png") == 0)
		return stbi_write_png_to_func(image_encode_sink_write, sink, w, h, ch, data,
					      w * ch);
	if (strcmp(ext, "jpg") == 0)
		return stbi_write_jpg_to_func(image_encode_sink_write, sink, w, h, ch, data, 90);
	if (strcmp(ext, "bmp") == 0)
		return stbi_write_bmp_to_func(image_encode_sink_write, sink, w, h, ch, data);
	if (strcmp(ext, "tga") == 0)
		return stbi_write_tga_to_func(image_encode_sink_write, sink, w, h, ch, data);
	return stbi_write_png_to_func(image_encode_sink_write, sink, w, h, ch, data, w * ch);
}

void image_encoded_cleanup(struct image_encoded *encoded)
{
	if (!encoded)
		return;
	free(encoded->base64);
	encoded->base64 = NULL;
	encoded->mime_type = NULL;
}

static void image_pixels_cleanup(unsigned char **pixels)
{
	if (!pixels)
		return;
	stbi_image_free(*pixels);
	*pixels = NULL;
}

static void image_resized_pixels_cleanup(unsigned char **pixels)
{
	if (!pixels)
		return;
	free(*pixels);
	*pixels = NULL;
}

static void image_encode_sink_cleanup(struct image_encode_sink *sink)
{
	nb_buf_cleanup(&sink->buf);
	sink->rc = 0;
}

int image_encode_base64_copy(const char *path, int max_dim, struct image_encoded *encoded)
{
	struct image_encoded candidate NB_CLEANUP(image_encoded_cleanup) = {0};
	struct image_encode_sink sink NB_CLEANUP(image_encode_sink_cleanup) = {0};
	unsigned char *pixels NB_CLEANUP(image_pixels_cleanup) = NULL;
	unsigned char *resized NB_CLEANUP(image_resized_pixels_cleanup) = NULL;
	const unsigned char *encoded_pixels;
	enum image_encode_format format;
	int width = 0;
	int height = 0;
	int channels = 0;
	int output_channels;
	int resized_width;
	int resized_height;
	int write_ok;
	int rc;

	if (!path || max_dim < 1 || !encoded)
		return NB_ERR_INVAL;
	if (encoded->base64 || encoded->mime_type)
		return NB_ERR_STATE;
	rc = image_validate_file(path);
	if (rc != 0)
		return rc;
	if (!stbi_info(path, &width, &height, &channels) ||
	    width <= 0 || height <= 0)
		return NB_ERR_FORMAT;
	format = image_encode_detect_format(path);
	if (width <= max_dim && height <= max_dim &&
	    format != IMAGE_ENCODE_OTHER) {
		rc = image_file_payload_copy(path, "", MORPH_MEDIA_MAX_FILE_BYTES,
					     &candidate.base64);
		if (rc != 0)
			return rc;
		candidate.mime_type = format == IMAGE_ENCODE_JPEG ? "image/jpeg" : "image/png";
	} else {
		output_channels = format == IMAGE_ENCODE_JPEG ? 3 : channels;
		if (output_channels < 1 || output_channels > 4)
			output_channels = 4;
		pixels = stbi_load(path, &width, &height, &channels, output_channels);
		if (!pixels)
			return NB_ERR_FORMAT;
		resized_width = width;
		resized_height = height;
		encoded_pixels = pixels;
		if (width > max_dim || height > max_dim) {
			double scale = width > height ? (double)max_dim / (double)width
						      : (double)max_dim / (double)height;
			size_t resized_bytes;

			resized_width = (int)((double)width * scale);
			resized_height = (int)((double)height * scale);
			if (resized_width < 1)
				resized_width = 1;
			if (resized_height < 1)
				resized_height = 1;
			rc = image_pixel_bytes(resized_width, resized_height, output_channels,
					       &resized_bytes);
			if (rc != 0)
				return rc;
			resized = malloc(resized_bytes);
			if (!resized)
				return NB_ERR_NOMEM;
			if (!stbir_resize_uint8_linear(pixels, width, height, 0, resized,
						       resized_width, resized_height, 0,
						       (stbir_pixel_layout)output_channels))
				return MORPH_ERR_PROCESSING;
			encoded_pixels = resized;
		}
		rc = (nb_buf_init(&sink.buf),
		      nb_buf_reserve(&sink.buf, (size_t)resized_width * (size_t)resized_height)
			      ? 0
			      : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
		if (format == IMAGE_ENCODE_JPEG) {
			write_ok = stbi_write_jpg_to_func(
				image_encode_sink_write, &sink, resized_width, resized_height,
				output_channels, encoded_pixels, IMAGE_JPEG_QUALITY);
			candidate.mime_type = "image/jpeg";
		} else {
			write_ok = stbi_write_png_to_func(
				image_encode_sink_write, &sink, resized_width, resized_height,
				output_channels, encoded_pixels, resized_width * output_channels);
			candidate.mime_type = "image/png";
		}
		if (!write_ok || sink.rc != 0 || sink.buf.len == 0)
			return sink.rc != 0 ? sink.rc : MORPH_ERR_PROCESSING;
		nb_buf payload NB_CLEANUP(nb_buf_destroy) = {0};
		if (nb_base64_encode(sink.buf.ptr, sink.buf.len, NB_B64_STD, true, &payload,
				     NULL) != NB_OK)
			return NB_ERR_NOMEM;
		candidate.base64 = strdup(nb_buf_cstr(&payload));
		if (!candidate.base64)
			return NB_ERR_NOMEM;
	}
	*encoded = candidate;
	memset(&candidate, 0, sizeof(candidate));
	return 0;
}

int image_encode_data_uri_copy(const char *path, int max_dim, char **uri)
{
	struct image_encoded encoded NB_CLEANUP(image_encoded_cleanup) = {0};
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc;

	if (!uri)
		return NB_ERR_INVAL;
	if (*uri)
		return NB_ERR_STATE;
	rc = image_encode_base64_copy(path, max_dim, &encoded);
	if (rc != 0)
		return rc;
	rc = (nb_buf_init(&buf),
	      nb_buf_reserve(&buf, strlen(encoded.base64) + strlen(encoded.mime_type) + 14)
		      ? 0
		      : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_printf(&buf, "data:%s;base64,%s", encoded.mime_type, encoded.base64)
			      ? 0
			      : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	*uri = (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
	return *uri ? 0 : NB_ERR_NOMEM;
}

int image_probe_size(const char *path, int *width, int *height)
{
	int w = 0;
	int h = 0;
	int ch = 0;

	if (!path || !width || !height)
		return NB_ERR_INVAL;

	if (!stbi_info(path, &w, &h, &ch) || w <= 0 || h <= 0)
		return NB_ERR_FORMAT;

	*width = w;
	*height = h;
	return 0;
}

int image_gen_normalize_reference_size(int src_w, int src_h,
				       int *out_w, int *out_h)
{
	long long pixels;
	double scale = 1.0;
	int w;
	int h;

	if (src_w <= 0 || src_h <= 0 || !out_w || !out_h)
		return NB_ERR_INVAL;

	pixels = (long long)src_w * (long long)src_h;
	if (pixels <= 0)
		return NB_ERR_INVAL;

	if (pixels < IMAGE_GEN_MIN_PIXELS)
		scale = sqrt((double)IMAGE_GEN_MIN_PIXELS / (double)pixels);
	else if (pixels > IMAGE_GEN_MAX_PIXELS)
		scale = sqrt((double)IMAGE_GEN_MAX_PIXELS / (double)pixels);

	w = (int)llround((double)src_w * scale);
	h = (int)llround((double)src_h * scale);
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;

	while ((long long)w * (long long)h < IMAGE_GEN_MIN_PIXELS) {
		if (src_w >= src_h)
			w++;
		else
			h++;
	}
	while ((long long)w * (long long)h > IMAGE_GEN_MAX_PIXELS) {
		if (src_w >= src_h && w > 1)
			w--;
		else if (h > 1)
			h--;
		else
			return NB_ERR_INVAL;
	}

	*out_w = w;
	*out_h = h;
	return 0;
}

int image_gen_format_size(char *buf, size_t buf_size, int width, int height)
{
	int n;

	if (!buf || buf_size == 0 || width <= 0 || height <= 0)
		return NB_ERR_INVAL;

	n = snprintf(buf, buf_size, "%dx%d", width, height);
	if (n < 0 || (size_t)n >= buf_size)
		return NB_ERR_NAME_TOO_LONG;
	return 0;
}

static void image_stbi_cleanup(unsigned char **image)
{
	stbi_image_free(*image);
	*image = NULL;
}

int image_resize_file_exact(const char *path, int width, int height)
{
	int src_w = 0;
	int src_h = 0;
	int src_ch = 0;
	int rc = 0;
	struct image_encode_sink sink NB_CLEANUP(image_encode_sink_cleanup) = {0};
	unsigned char *src NB_CLEANUP(image_stbi_cleanup) = NULL;
	unsigned char *dst NB_CLEANUP(nb_ucharp_cleanup) = NULL;
	const char *ext;
	int wrc;

	if (!path || width <= 0 || height <= 0)
		return NB_ERR_INVAL;

	size_t dst_bytes;
	rc = image_pixel_bytes(width, height, 4, &dst_bytes);
	if (rc == 0)
		rc = image_validate_file(path);
	if (rc != 0)
		return rc;
	src = stbi_load(path, &src_w, &src_h, &src_ch, 0);
	if (!src)
		return NB_ERR_FORMAT;

	dst = malloc((size_t)width * (size_t)height * (size_t)src_ch);
	if (!dst) {
		rc = NB_ERR_NOMEM;
		return rc;
	}

	if (!stbir_resize_uint8_linear(src, src_w, src_h, 0,
				       dst, width, height, 0,
				       (stbir_pixel_layout)src_ch)) {
		rc = MORPH_ERR_PROCESSING;
		return rc;
	}

	ext = image_out_ext(path);
	wrc = image_write_by_ext(&sink, ext, width, height, src_ch, dst);
	if (sink.rc != 0)
		return sink.rc;
	if (!wrc || nb_buf_empty(&sink.buf))
		return NB_ERR_IO;
	return nb_file_write_all_atomic(path, nb_buf_cstr(&sink.buf), nb_buf_len(&sink.buf), NULL,
					NULL);
}

static void base64_file_cleanup(FILE **file)
{
	if (file && *file) {
		fclose(*file);
		*file = NULL;
	}
}

int image_file_payload_copy(const char *path, const char *prefix, size_t max_bytes, char **out)
{
	unsigned char chunk[BUFSIZ - BUFSIZ % 3];
	struct stat st;
	FILE *file NB_CLEANUP(base64_file_cleanup) = NULL;
	nb_buf payload NB_CLEANUP(nb_buf_destroy) = {0};
	if (!path || !prefix || !out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_STATE;
	file = fopen(path, "rb");
	if (!file)
		return nb_status_from_errno(errno, NULL);
	if (fstat(fileno(file), &st) != 0)
		return nb_status_from_errno(errno, NULL);
	if (!S_ISREG(st.st_mode) || st.st_size <= 0)
		return NB_ERR_INVAL;
	if ((uintmax_t)st.st_size > max_bytes)
		return NB_ERR_TOO_BIG;
	if (!nb_buf_append_cstr(&payload, prefix))
		return NB_ERR_NOMEM;
	size_t remaining = (size_t)st.st_size;
	while (remaining > 0) {
		size_t wanted = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
		size_t got = fread(chunk, 1, wanted, file);
		if (got != wanted)
			return ferror(file) && errno ? nb_status_from_errno(errno, NULL)
						     : NB_ERR_IO;
		if (nb_base64_encode(chunk, got, NB_B64_STD, true, &payload, NULL) != NB_OK)
			return NB_ERR_NOMEM;
		remaining -= got;
	}
	if (fgetc(file) != EOF)
		return NB_ERR_TOO_BIG;
	if (ferror(file))
		return errno ? nb_status_from_errno(errno, NULL) : NB_ERR_IO;
	if (fclose(file) != 0) {
		file = NULL;
		return errno ? nb_status_from_errno(errno, NULL) : NB_ERR_IO;
	}
	file = NULL;
	char *copy = strdup(nb_buf_cstr(&payload));
	if (!copy)
		return NB_ERR_NOMEM;
	*out = copy;
	return 0;
}
