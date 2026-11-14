#include "image_output.h"
#include <stdlib.h>
#include "nb/cleanup.h"

#include "agent/tool_result.h"
#include "models/media_output.h"
#include "permission/context.h"
#include "nb/buf.h"
#include "errors.h"
#include "nb/file.h"
#include "media/image.h"
#include "stb_image_write.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

struct image_output_sink {
	nb_buf bytes;
	size_t limit;
	int error;
};

const char *tool_image_path_format(const char *path)
{
	const char *extension = path ? strrchr(path, '.') : NULL;

	if (extension) {
		if (strcasecmp(extension, ".jpg") == 0 || strcasecmp(extension, ".jpeg") == 0)
			return "jpg";
		if (strcasecmp(extension, ".bmp") == 0)
			return "bmp";
		if (strcasecmp(extension, ".tga") == 0)
			return "tga";
	}
	return "png";
}

static void image_output_append(void *data, void *bytes, int length)
{
	struct image_output_sink *sink = data;

	if (sink->error)
		return;
	if (length < 0 || (size_t)length > sink->limit - sink->bytes.len) {
		sink->error = NB_ERR_TOO_BIG;
		return;
	}
	sink->error = (nb_buf_append(&sink->bytes, bytes, (size_t)length) ? 0 : NB_ERR_NOMEM);
}

static int image_output_encode(const struct tool_image_output *image,
			       struct image_output_sink *sink)
{
	size_t pixels;
	int encoded;
	int rc = image_pixel_bytes(image->width, image->height, 4, &pixels);

	if (rc != 0)
		return rc;
	if (image->channels < 1 || image->channels > 4)
		return NB_ERR_INVAL;
	if (pixels > (SIZE_MAX - BUFSIZ) / 2)
		return NB_ERR_RANGE;
	/* Two RGBA images plus headers cover PNG, JPEG, BMP and TGA expansion. */
	sink->limit = pixels * 2 + BUFSIZ;
	rc = (nb_buf_init(&sink->bytes), nb_buf_reserve(&sink->bytes, BUFSIZ) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	if (strcmp(image->format, "png") == 0)
		encoded = stbi_write_png_to_func(image_output_append, sink, image->width,
						 image->height, image->channels, image->pixels,
						 image->width * image->channels);
	else if (strcmp(image->format, "jpg") == 0)
		/* stb ignores alpha, retaining the original RGB conversion behavior. */
		encoded = stbi_write_jpg_to_func(image_output_append, sink, image->width,
						 image->height, image->channels, image->pixels,
						 image->quality);
	else if (strcmp(image->format, "bmp") == 0)
		encoded = stbi_write_bmp_to_func(image_output_append, sink, image->width,
						 image->height, image->channels, image->pixels);
	else if (strcmp(image->format, "tga") == 0)
		encoded = stbi_write_tga_to_func(image_output_append, sink, image->width,
						 image->height, image->channels, image->pixels);
	else
		return NB_ERR_INVAL;
	if (sink->error)
		return sink->error;
	return encoded && sink->bytes.len ? 0 : MORPH_ERR_PROCESSING;
}

static int image_output_error(struct tool_result *result, int error)
{
	int rc;

	if (error == NB_ERR_NOT_PERMITTED || error == NB_ERR_PERMISSION)
		rc = tool_result_error_copy(
			result, "permission_denied",
			"write path outside output directory: permission denied");
	else
		rc = tool_result_error_format(
			result, "image_write_failed", "failed to write output: %s",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, error));
	return rc != 0 ? rc : error;
}

int tool_image_write_output(struct tool_context *context, const char *requested_path,
			    const char *prefix, const struct tool_image_output *image,
			    struct tool_result *result, char *path, size_t path_size)
{
	struct image_output_sink sink = {0};
	struct tool_image_output encoded_image;
	int explicit_path = requested_path && *requested_path;
	int rc;

	if (!image || !image->pixels || (!image->format && !explicit_path) || !result || !path ||
	    !path_size)
		return NB_ERR_INVAL;
	path[0] = '\0';
	if (explicit_path) {
		if (context) {
			rc = tool_context_authorize_path(context, TOOL_PATH_WRITE, requested_path,
							 path, path_size);
			if (rc != 0) {
				path[0] = '\0';
				return image_output_error(result, rc);
			}
		} else {
			if (strlen(requested_path) >= path_size)
				return image_output_error(result, NB_ERR_NAME_TOO_LONG);
			memcpy(path, requested_path, strlen(requested_path) + 1);
		}
	}
	encoded_image = *image;
	if (!encoded_image.format)
		encoded_image.format = tool_image_path_format(path);
	rc = image_output_encode(&encoded_image, &sink);
	if (rc == 0) {
		if (explicit_path)
			rc = nb_file_write_all_atomic(
				path, sink.bytes.ptr, sink.bytes.len,
				&(nb_file_write_options){.new_file_mode = 0600, .sync_dir = true},
				NULL);
		else
			rc = media_output_write(
				context ? tool_context_output_dir_borrow(context) : NULL, prefix,
				image->format, sink.bytes.ptr, sink.bytes.len, path, path_size);
	}
	nb_buf_cleanup(&sink.bytes);
	if (rc != 0) {
		path[0] = '\0';
		return image_output_error(result, rc);
	}
	return 0;
}
