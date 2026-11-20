#include "sapi/cli/adapters/media/image_probe.h"
#include "errors.h"
#include "stb_image.h"
#include <errno.h>

int cli_media_image_probe_info(const char *absolute_path, int *width, int *height)
{
	int channels;

	if (!absolute_path || !width || !height)
		return NB_ERR_INVAL;

	if (!stbi_info(absolute_path, width, height, &channels))
		return NB_ERR_FORMAT;

	return 0;
}

int cli_media_image_probe_file(FILE *file, int *width, int *height)
{
	if (!file || !width || !height)
		return NB_ERR_INVAL;

	int channels = 0;
	return stbi_info_from_file(file, width, height, &channels) ? NB_OK : NB_ERR_FORMAT;
}
