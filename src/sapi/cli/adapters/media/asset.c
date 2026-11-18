#include "sapi/cli/adapters/media/asset.h"
#include "media/image.h"
#include "stb_image.h"
#include <nb/cleanup.h>
#include <limits.h>

int cli_media_native_available(void)
{
	return nt_media_protocol_select(NULL) >= NT_MEDIA_PROTOCOL_KITTY;
}

static void cli_asset_pixels_cleanup(unsigned char **pixels)
{
	stbi_image_free(*pixels);
	*pixels = NULL;
}

int cli_media_asset_from_png(struct nt_media_asset **out, const unsigned char *bytes, size_t length)
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
	unsigned char *pixels NB_CLEANUP(cli_asset_pixels_cleanup) =
		stbi_load_from_memory(bytes, (int)length, &width, &height, &channels, 4);
	if (!pixels)
		return NB_ERR_FORMAT;
	return nt_media_asset_create_rgba(out, pixels, width, height);
}
