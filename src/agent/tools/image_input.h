#ifndef MORPH_TOOL_IMAGE_INPUT_H
#define MORPH_TOOL_IMAGE_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

struct tool_context;
struct tool_result;

struct tool_image_input {
	unsigned char *pixels;
	int width;
	int height;
	int channels;
};

/* Authorize before decoding into a zero-initialized owner. Failure leaves it empty.
 * Cleanup releases decoded pixels and resets dimensions. */
void tool_image_input_cleanup(struct tool_image_input *image);
int tool_image_load_input(struct tool_context *context, const char *path,
			  struct tool_result *result, struct tool_image_input *image);

#ifdef __cplusplus
}
#endif

#endif
