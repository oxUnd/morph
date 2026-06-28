#include "image_util.h"
#include "base64.h"
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

char *image_encode_base64(const char *path, int max_dim)
{
	if (!path || max_dim < 1)
		return NULL;

	int w, h, ch;
	unsigned char *pixels = stbi_load(path, &w, &h, &ch, 4);
	if (!pixels)
		return NULL;

	int rw = w, rh = h;
	unsigned char *resized = pixels;

	if (w > max_dim || h > max_dim) {
		double scale = (w > h) ? (double)max_dim / w : (double)max_dim / h;
		rw = (int)(w * scale);
		rh = (int)(h * scale);
		if (rw < 1) rw = 1;
		if (rh < 1) rh = 1;

		resized = malloc((size_t)rw * (size_t)rh * 4);
		if (!resized) {
			stbi_image_free(pixels);
			return NULL;
		}

		for (int y = 0; y < rh; y++) {
			for (int x = 0; x < rw; x++) {
				int sx = x * w / rw;
				int sy = y * h / rh;
				memcpy(resized + (y * rw + x) * 4,
				       pixels + (sy * w + sx) * 4, 4);
			}
		}

		stbi_image_free(pixels);
	}

	int png_len = 0;
	unsigned char *png = stbi_write_png_to_mem(resized, 0, rw, rh, 4, &png_len);

	if (resized != pixels)
		free(resized);

	if (!png || png_len <= 0)
		return NULL;

	char *b64 = base64_encode(png, (size_t)png_len);
	free(png);
	return b64;
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
