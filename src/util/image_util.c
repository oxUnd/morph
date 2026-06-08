#include "image_util.h"
#include "base64.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

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
