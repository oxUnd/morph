#ifndef MORPH_TOOL_IMAGE_INPUT_H
#define MORPH_TOOL_IMAGE_INPUT_H

struct tool_context;
struct tool_result;

struct tool_image_input {
	unsigned char *pixels;
	int width;
	int height;
	int channels;
};

/* Authorize before decoding. The caller releases successful pixels with stbi_image_free. */
int tool_image_load_input(struct tool_context *context, const char *path,
			  struct tool_result *result, struct tool_image_input *image);

#endif
