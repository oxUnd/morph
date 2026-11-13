#include <morph/tui/media.h>
#include "util/cleanup.h"
#include "util/image_util.h"
#include "util/error.h"
#include "stb_image.h"
#include "stb_image_write.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct morph_tui_media_asset {
	unsigned char *pixels;
	int width, height;
	atomic_uint references;
	uint64_t key;
};

static uint64_t render_media_hash(uint64_t value, const void *data, size_t length)
{
	const unsigned char *bytes = data;
	for (size_t i = 0; i < length; i++) {
		value ^= bytes[i];
		value *= UINT64_C(1099511628211);
	}
	return value;
}

static void render_media_asset_cleanup(struct morph_tui_media_asset **asset)
{
	morph_tui_media_asset_destroy(*asset);
	*asset = NULL;
}

static struct morph_tui_media_asset *render_media_asset_steal(struct morph_tui_media_asset **owner)
{
	struct morph_tui_media_asset *asset = *owner;
	*owner = NULL;
	return asset;
}

int morph_tui_media_asset_create_rgba_copy(struct morph_tui_media_asset **out,
					  const unsigned char *pixels, int width, int height)
{
	if (!out || *out || !pixels)
		return -EINVAL;
	size_t bytes = 0;
	int rc = image_pixel_bytes(width, height, 4, &bytes);
	if (rc)
		return rc;
	struct morph_tui_media_asset *asset MORPH_CLEANUP(render_media_asset_cleanup) =
		calloc(1, sizeof(*asset));
	if (!asset)
		return -ENOMEM;
	atomic_init(&asset->references, 1);
	asset->pixels = malloc(bytes);
	if (!asset->pixels)
		return -ENOMEM;
	memcpy(asset->pixels, pixels, bytes);
	asset->width = width;
	asset->height = height;
	asset->key = render_media_hash(UINT64_C(14695981039346656037), &width, sizeof(width));
	asset->key = render_media_hash(asset->key, &height, sizeof(height));
	asset->key = render_media_hash(asset->key, pixels, bytes);
	*out = render_media_asset_steal(&asset);
	return 0;
}

static void render_media_pixels_cleanup(unsigned char **pixels)
{
	stbi_image_free(*pixels);
	*pixels = NULL;
}

int morph_tui_media_asset_create_png_copy(struct morph_tui_media_asset **out,
					 const unsigned char *bytes, size_t length)
{
	if (!out || *out || !bytes || !length || length > INT_MAX)
		return -EINVAL;
	if (length < 8 || memcmp(bytes, "\211PNG\r\n\032\n", 8))
		return MORPH_ERR_FORMAT;
	int width = 0, height = 0, channels = 0;
	if (!stbi_info_from_memory(bytes, (int)length, &width, &height, &channels))
		return MORPH_ERR_FORMAT;
	size_t size = 0;
	int rc = image_pixel_bytes(width, height, 4, &size);
	if (rc)
		return rc;
	unsigned char *pixels MORPH_CLEANUP(render_media_pixels_cleanup) =
		stbi_load_from_memory(bytes, (int)length, &width, &height, &channels, 4);
	if (!pixels)
		return MORPH_ERR_FORMAT;
	return morph_tui_media_asset_create_rgba_copy(out, pixels, width, height);
}

struct morph_tui_media_asset *morph_tui_media_asset_retain(struct morph_tui_media_asset *asset)
{
	if (asset)
		atomic_fetch_add_explicit(&asset->references, 1u, memory_order_relaxed);
	return asset;
}

void morph_tui_media_asset_destroy(struct morph_tui_media_asset *asset)
{
	if (!asset || atomic_fetch_sub_explicit(&asset->references, 1u, memory_order_acq_rel) != 1u)
		return;
	free(asset->pixels);
	free(asset);
}

int morph_tui_media_asset_width(const struct morph_tui_media_asset *asset)
{
	return asset ? asset->width : 0;
}

int morph_tui_media_asset_height(const struct morph_tui_media_asset *asset)
{
	return asset ? asset->height : 0;
}

const unsigned char *morph_tui_media_asset_pixels_borrow(const struct morph_tui_media_asset *asset)
{
	return asset ? asset->pixels : NULL;
}

uint64_t morph_tui_media_asset_key(const struct morph_tui_media_asset *asset)
{
	return asset ? asset->key : 0;
}

static void render_media_png_write(void *context, void *bytes, int length)
{
	morph_buf_t *out = context;
	if (length > 0)
		(void)morph_buf_append(out, bytes, (size_t)length);
}

int morph_tui_media_asset_png_append(const struct morph_tui_media_asset *asset,
				     int x, int y, int width, int height, morph_buf_t *out)
{
	if (!asset || !out || x < 0 || y < 0 || width < 1 || height < 1 ||
	    x > asset->width - width || y > asset->height - height)
		return -EINVAL;
	const unsigned char *start = asset->pixels +
		((size_t)y * (size_t)asset->width + (size_t)x) * 4u;
	int success = stbi_write_png_to_func(render_media_png_write, out, width, height, 4,
		start, asset->width * 4);
	return out->failed ? out->failed : success ? 0 : MORPH_ERR_FORMAT;
}
