#ifndef MORPH_TOOL_IMAGE_OUTPUT_H
#define MORPH_TOOL_IMAGE_OUTPUT_H

#include <stddef.h>

struct tool_context;
struct tool_result;

struct tool_image_output {
	const unsigned char *pixels;
	int width;
	int height;
	int channels;
	/* NULL selects the authorized explicit path's format. */
	const char *format;
	int quality;
};

const char *tool_image_path_format(const char *path);
/* Pixels and strings are borrowed. Encoded bytes belong to this call and are
 * bounded by the validated pixel size. Only a completed file is published. */
int tool_image_write_output(struct tool_context *context, const char *requested_path,
			    const char *prefix, const struct tool_image_output *image,
			    struct tool_result *result, char *path, size_t path_size);

#endif
