#include <morph/tui/media.h>
#include "nb/cleanup.h"
#include "media/image.h"
#include "errors.h"
#include "stb_image.h"
#include "stb_image_write.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct morph_tui_media_asset {
	unsigned char *pixels;
	void (*release_pixels)(void *);
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

static int render_media_asset_pixels_take(struct morph_tui_media_asset **out,
					 unsigned char **pixels, int width, int height,
					 void (*release_pixels)(void *))
{
	struct morph_tui_media_asset *asset NB_CLEANUP(render_media_asset_cleanup) =
		calloc(1, sizeof(*asset));
	if (!asset)
		return NB_ERR_NOMEM;
	atomic_init(&asset->references, 1);
	asset->pixels = *pixels;
	asset->release_pixels = release_pixels;
	asset->width = width;
	asset->height = height;
	asset->key = render_media_hash(UINT64_C(14695981039346656037), &width, sizeof(width));
	asset->key = render_media_hash(asset->key, &height, sizeof(height));
	asset->key = render_media_hash(asset->key, *pixels, (size_t)width * (size_t)height * 4u);
	*pixels = NULL;
	*out = render_media_asset_steal(&asset);
	return NB_OK;
}

int morph_tui_media_asset_create_rgba_copy(struct morph_tui_media_asset **out,
					  const unsigned char *pixels, int width, int height)
{
	if (!out || *out || !pixels)
		return NB_ERR_INVAL;
	size_t bytes = 0;
	int rc = image_pixel_bytes(width, height, 4, &bytes);
	if (rc)
		return rc;
	unsigned char *copy NB_CLEANUP(nb_ucharp_cleanup) = malloc(bytes);
	if (!copy)
		return NB_ERR_NOMEM;
	memcpy(copy, pixels, bytes);
	return render_media_asset_pixels_take(out, &copy, width, height, free);
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
		return NB_ERR_INVAL;
	if (length < 8 || memcmp(bytes, "\211PNG\r\n\032\n", 8))
		return NB_ERR_FORMAT;
	int width = 0, height = 0, channels = 0;
	if (!stbi_info_from_memory(bytes, (int)length, &width, &height, &channels))
		return NB_ERR_FORMAT;
	size_t size = 0;
	int rc = image_pixel_bytes(width, height, 4, &size);
	if (rc)
		return rc;
	unsigned char *pixels NB_CLEANUP(render_media_pixels_cleanup) =
		stbi_load_from_memory(bytes, (int)length, &width, &height, &channels, 4);
	if (!pixels)
		return NB_ERR_FORMAT;
	return render_media_asset_pixels_take(out, &pixels, width, height, stbi_image_free);
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
	if (asset->pixels)
		asset->release_pixels(asset->pixels);
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
	nb_buf *out = context;
	if (length > 0)
		(void)nb_buf_append(out, bytes, (size_t)length);
}

int morph_tui_media_asset_png_append(const struct morph_tui_media_asset *asset, int x, int y,
				     int width, int height, nb_buf *out)
{
	if (!asset || !out || x < 0 || y < 0 || width < 1 || height < 1 ||
	    x > asset->width - width || y > asset->height - height)
		return NB_ERR_INVAL;
	const unsigned char *start = asset->pixels +
		((size_t)y * (size_t)asset->width + (size_t)x) * 4u;
	int success = stbi_write_png_to_func(render_media_png_write, out, width, height, 4,
		start, asset->width * 4);
	return nb_buf_status(out) ? nb_buf_status(out) : success ? 0 : NB_ERR_FORMAT;
}
