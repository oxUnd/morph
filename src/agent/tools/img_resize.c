#include "img_resize.h"
#include "cJSON.h"
#include "nb/cleanup.h"
#include "image_input.h"
#include "image_output.h"
#include "agent/tool_runtime.h"
#include "agent/tool_context.h"
#include "nb/log.h"
#include "errors.h"
#include "media/image.h"
#include "cJSON.h"
#include "stb_image.h"

#include "stb_image_resize2.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int img_resize_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	if (!result)
		return NB_ERR_INVAL;

	struct tool_context *tctx = user_data;

	cJSON *root = cJSON_Parse(args_json);
	if (!root) {
		(void)tool_result_success_json_text_copy(result, "{\"error\":\"invalid JSON\"}");
		return NB_ERR_INVAL;
	}

	cJSON *fp = cJSON_GetObjectItem(root, "file_path");
	cJSON *cw = cJSON_GetObjectItem(root, "width");
	cJSON *chh = cJSON_GetObjectItem(root, "height");
	cJSON *out = cJSON_GetObjectItem(root, "output_path");

	const char *file_path = cJSON_IsString(fp) ? fp->valuestring : NULL;
	if ((cJSON_IsNumber(cw) && (!isfinite(cw->valuedouble) || cw->valuedouble < 0 ||
				    cw->valuedouble > MORPH_IMAGE_MAX_DIMENSION)) ||
	    (cJSON_IsNumber(chh) && (!isfinite(chh->valuedouble) || chh->valuedouble < 0 ||
				     chh->valuedouble > MORPH_IMAGE_MAX_DIMENSION))) {
		cJSON_Delete(root);
		(void)tool_result_error_copy(result, "image_too_large", "Invalid image dimensions");
		return NB_ERR_TOO_BIG;
	}
	int target_w = cJSON_IsNumber(cw) ? (int)cw->valuedouble : 0;
	int target_h = cJSON_IsNumber(chh) ? (int)chh->valuedouble : 0;
	const char *out_path_in = cJSON_IsString(out) ? out->valuestring : NULL;

	if (!file_path || (target_w <= 0 && target_h <= 0)) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"missing 'file_path' or 'width'/'height' parameter. "
				"Usage: img_resize({\\\"file_path\\\": \\\"img.png\\\", "
				"\\\"width\\\": 800, \\\"height\\\": 600})\"}");
		return NB_ERR_INVAL;
	}

	struct tool_image_input input NB_CLEANUP(tool_image_input_cleanup) = {0};
	int input_rc = tool_image_load_input(tctx, file_path, result, &input);

	if (input_rc != 0) {
		cJSON_Delete(root);
		return input_rc;
	}
	int src_w = input.width, src_h = input.height, src_ch = input.channels;
	unsigned char *src = input.pixels;

	/* Maintain aspect ratio if only one dim is given */
	if (target_w <= 0)
		target_w = (int)((double)src_w * target_h / src_h);
	if (target_h <= 0)
		target_h = (int)((double)src_h * target_w / src_w);
	if (target_w <= 0)
		target_w = 1;
	if (target_h <= 0)
		target_h = 1;

	size_t dst_bytes;
	int size_rc = image_pixel_bytes(target_w, target_h, src_ch, &dst_bytes);
	if (size_rc != 0) {
		tool_image_input_cleanup(&input);
		cJSON_Delete(root);
		(void)tool_result_error_copy(result, "image_too_large",
					     "Image exceeds pixel limit");
		return size_rc;
	}
	unsigned char *dst = malloc(dst_bytes);
	if (!dst) {
		tool_image_input_cleanup(&input);
		cJSON_Delete(root);
		(void)tool_result_success_json_text_copy(result, "{\"error\":\"oom\"}");
		return NB_ERR_NOMEM;
	}

	if (!stbir_resize_uint8_linear(src, src_w, src_h, 0, dst, target_w, target_h, 0,
				       (stbir_pixel_layout)src_ch)) {
		tool_image_input_cleanup(&input);
		free(dst);
		cJSON_Delete(root);
		(void)tool_result_success_json_text_copy(result, "{\"error\":\"resize failed\"}");
		return MORPH_ERR_PROCESSING;
	}
	tool_image_input_cleanup(&input);

	char final_path[PATH_MAX];
	struct tool_image_output output = {
		.pixels = dst,
		.width = target_w,
		.height = target_h,
		.channels = src_ch,
		.format = out_path_in && *out_path_in ? NULL : tool_image_path_format(file_path),
		.quality = 90,
	};
	int output_rc = tool_image_write_output(tctx, out_path_in, "img_resized", &output, result,
						final_path, sizeof(final_path));

	free(dst);
	if (output_rc != 0) {
		cJSON_Delete(root);
		return output_rc;
	}

	size_t msg_len = strlen(final_path) + 64;
	char *msg NB_CLEANUP(nb_charp_cleanup) = malloc(msg_len);
	if (!msg) {
		cJSON_Delete(root);
		return NB_ERR_NOMEM;
	}
	snprintf(msg, msg_len, "image resized: %s (%dx%d)", final_path, target_w, target_h);
	(void)tool_result_success_json_text_copy(result, msg);
	(void)tool_result_add_image_copy(result, final_path, target_w, target_h);
	nb_log_write(result && result->runtime ? result->runtime->http.logger : NULL, NB_LOG_DEBUG,
		     "img_resize: %s", msg);

	cJSON_Delete(root);
	return 0;
}

int img_resize_init(struct tool_registry *reg, struct tool_context *tctx)
{
	if (!reg)
		return NB_ERR_INVAL;
	return tool_register_borrow(
		reg,
		&(struct tool_spec){
			.origin = TOOL_ORIGIN_BUILTIN,
			.flags = TOOL_FLAG_MEDIA,
			.name = "img_resize",
			.description =
				"Resize an image to the given width/height. "
				"Provide file_path, width, height, optional output_path. "
				"If only one of width/height is given, the other is computed "
				"to preserve aspect ratio.",
			.input_schema =
				"{\"type\":\"object\",\"properties\":"
				"{\"file_path\":{\"type\":\"string\",\"description\":\"Path to the "
				"image file\"},"
				"\"width\":{\"type\":\"integer\",\"description\":\"Target width in "
				"pixels\"},"
				"\"height\":{\"type\":\"integer\",\"description\":\"Target height "
				"in pixels\"},"
				"\"output_path\":{\"type\":\"string\",\"description\":\"Output "
				"file path (optional)\"}},\"required\":[\"file_path\"]}",
			.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
			.exec = img_resize_exec,
			.user_data = tctx,
			.user_data_destroy = NULL});
}
