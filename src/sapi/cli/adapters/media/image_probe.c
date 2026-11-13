#include "sapi/cli/adapters/media/image_probe.h"
#include "util/error.h"
#include "stb_image.h"
#include <errno.h>

int cli_media_image_probe_info(const char *absolute_path, int *width, int *height)
{
	int channels;

	if (!absolute_path || !width || !height)
		return -EINVAL;
	if (!stbi_info(absolute_path, width, height, &channels))
		return MORPH_ERR_FORMAT;
	return 0;
}
