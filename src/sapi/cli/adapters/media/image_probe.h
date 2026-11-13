#ifndef MORPH_CLI_ADAPTERS_MEDIA_IMAGE_PROBE_H
#define MORPH_CLI_ADAPTERS_MEDIA_IMAGE_PROBE_H

/* Reads image dimensions from the file header without decoding pixels.
 * Returns 0 when the file is a supported image and MORPH_ERR_FORMAT when it is
 * not; width/height are only written on success. */
int cli_media_image_probe_info(const char *absolute_path, int *width, int *height);

#endif
