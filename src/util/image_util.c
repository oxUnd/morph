#include "image_util.h"
#include "base64.h"
#include "buf.h"
#include "error.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#define IMAGE_GEN_MIN_PIXELS (2560LL * 1440LL)
#define IMAGE_GEN_MAX_PIXELS (4096LL * 4096LL)
#define IMAGE_JPEG_QUALITY 88

enum image_encode_format {
	IMAGE_ENCODE_PNG,
	IMAGE_ENCODE_JPEG,
	IMAGE_ENCODE_OTHER,
};

struct image_encode_sink {
	morph_buf_t buf;
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
	sink->rc = morph_buf_append(&sink->buf, (const char *)data,
				    (size_t)size);
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

static int image_write_by_ext(const char *path, const char *ext, int w, int h,
			      int ch, const unsigned char *data)
{
	if (strcmp(ext, "png") == 0)
		return stbi_write_png(path, w, h, ch, data, w * ch);
	if (strcmp(ext, "jpg") == 0)
		return stbi_write_jpg(path, w, h, ch, data, 90);
	if (strcmp(ext, "bmp") == 0)
		return stbi_write_bmp(path, w, h, ch, data);
	if (strcmp(ext, "tga") == 0)
		return stbi_write_tga(path, w, h, ch, data);
	return stbi_write_png(path, w, h, ch, data, w * ch);
}

void image_encoded_cleanup(struct image_encoded *encoded)
{
	if (!encoded)
		return;
	free(encoded->base64);
	encoded->base64 = NULL;
	encoded->mime_type = NULL;
}

int image_encode_base64(const char *path, int max_dim,
			struct image_encoded *encoded)
{
	struct image_encode_sink sink;
	enum image_encode_format format;
	unsigned char *pixels = NULL;
	unsigned char *resized = NULL;
	int width = 0;
	int height = 0;
	int channels = 0;
	int output_channels;
	int resized_width;
	int resized_height;
	int write_ok;
	int rc;

	if (!path || max_dim < 1 || !encoded)
		MORPH_RETURN(-EINVAL);
	memset(encoded, 0, sizeof(*encoded));
	if (!stbi_info(path, &width, &height, &channels) ||
	    width <= 0 || height <= 0)
		MORPH_RETURN(MORPH_ERR_FORMAT);
	format = image_encode_detect_format(path);
	if (width <= max_dim && height <= max_dim &&
	    format != IMAGE_ENCODE_OTHER) {
		encoded->base64 = base64_encode_file(path);
		if (!encoded->base64)
			MORPH_RETURN(-EIO);
		encoded->mime_type = format == IMAGE_ENCODE_JPEG
			? "image/jpeg" : "image/png";
		return 0;
	}

	output_channels = format == IMAGE_ENCODE_JPEG ? 3 : channels;
	if (output_channels < 1 || output_channels > 4)
		output_channels = 4;
	pixels = stbi_load(path, &width, &height, &channels, output_channels);
	if (!pixels)
		MORPH_RETURN(MORPH_ERR_FORMAT);
	resized_width = width;
	resized_height = height;
	resized = pixels;
	if (width > max_dim || height > max_dim) {
		double scale = width > height
			? (double)max_dim / (double)width
			: (double)max_dim / (double)height;

		resized_width = (int)((double)width * scale);
		resized_height = (int)((double)height * scale);
		if (resized_width < 1)
			resized_width = 1;
		if (resized_height < 1)
			resized_height = 1;
		resized = malloc((size_t)resized_width *
				 (size_t)resized_height * (size_t)output_channels);
		if (!resized) {
			stbi_image_free(pixels);
			MORPH_RETURN(-ENOMEM);
		}
		if (!stbir_resize_uint8_linear(
				pixels, width, height, 0, resized,
				resized_width, resized_height, 0,
				(stbir_pixel_layout)output_channels)) {
			free(resized);
			stbi_image_free(pixels);
			MORPH_RETURN(MORPH_ERR_PROCESSING);
		}
	}

	memset(&sink, 0, sizeof(sink));
	rc = morph_buf_init(&sink.buf,
			    (size_t)resized_width * (size_t)resized_height);
	if (rc != 0) {
		if (resized != pixels)
			free(resized);
		stbi_image_free(pixels);
		return rc;
	}
	if (format == IMAGE_ENCODE_JPEG) {
		write_ok = stbi_write_jpg_to_func(
			image_encode_sink_write, &sink, resized_width,
			resized_height, output_channels, resized,
			IMAGE_JPEG_QUALITY);
		encoded->mime_type = "image/jpeg";
	} else {
		write_ok = stbi_write_png_to_func(
			image_encode_sink_write, &sink, resized_width,
			resized_height, output_channels, resized,
			resized_width * output_channels);
		encoded->mime_type = "image/png";
	}
	if (resized != pixels)
		free(resized);
	stbi_image_free(pixels);
	if (!write_ok || sink.rc != 0 || sink.buf.len == 0) {
		rc = sink.rc != 0 ? sink.rc : MORPH_ERR_PROCESSING;
		morph_buf_cleanup(&sink.buf);
		encoded->mime_type = NULL;
		MORPH_RETURN(rc);
	}
	encoded->base64 = base64_encode(
		(const unsigned char *)sink.buf.data, sink.buf.len);
	morph_buf_cleanup(&sink.buf);
	if (!encoded->base64) {
		encoded->mime_type = NULL;
		MORPH_RETURN(-ENOMEM);
	}
	return 0;
}

int image_probe_size(const char *path, int *width, int *height)
{
	int w = 0;
	int h = 0;
	int ch = 0;

	if (!path || !width || !height)
		MORPH_RETURN(-EINVAL);

	if (!stbi_info(path, &w, &h, &ch) || w <= 0 || h <= 0)
		MORPH_RETURN(MORPH_ERR_FORMAT);

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
		MORPH_RETURN(-EINVAL);

	pixels = (long long)src_w * (long long)src_h;
	if (pixels <= 0)
		MORPH_RETURN(-EINVAL);

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
			MORPH_RETURN(-EINVAL);
	}

	*out_w = w;
	*out_h = h;
	return 0;
}

int image_gen_format_size(char *buf, size_t buf_size, int width, int height)
{
	int n;

	if (!buf || buf_size == 0 || width <= 0 || height <= 0)
		MORPH_RETURN(-EINVAL);

	n = snprintf(buf, buf_size, "%dx%d", width, height);
	if (n < 0 || (size_t)n >= buf_size)
		MORPH_RETURN(-ENAMETOOLONG);
	return 0;
}

int image_resize_file_exact(const char *path, int width, int height)
{
	int src_w = 0;
	int src_h = 0;
	int src_ch = 0;
	int rc = 0;
	char tmp_path[PATH_MAX];
	unsigned char *src = NULL;
	unsigned char *dst = NULL;
	const char *ext;
	int wrc;
	int n;

	if (!path || width <= 0 || height <= 0)
		MORPH_RETURN(-EINVAL);

	src = stbi_load(path, &src_w, &src_h, &src_ch, 0);
	if (!src)
		MORPH_RETURN(MORPH_ERR_FORMAT);

	dst = malloc((size_t)width * (size_t)height * (size_t)src_ch);
	if (!dst) {
		rc = -ENOMEM;
		goto out;
	}

	if (!stbir_resize_uint8_linear(src, src_w, src_h, 0,
				       dst, width, height, 0,
				       (stbir_pixel_layout)src_ch)) {
		rc = MORPH_ERR_PROCESSING;
		goto out;
	}

	n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
	if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
		rc = -ENAMETOOLONG;
		goto out;
	}
	ext = image_out_ext(path);
	wrc = image_write_by_ext(tmp_path, ext, width, height, src_ch, dst);
	if (!wrc) {
		rc = -EIO;
		goto out;
	}
	if (rename(tmp_path, path) < 0) {
		rc = -errno;
		(void)unlink(tmp_path);
		goto out;
	}

out:
	if (src)
		stbi_image_free(src);
	free(dst);
	if (rc < 0)
		MORPH_RETURN(rc);
	return 0;
}
