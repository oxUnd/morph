#include "img_convert.h"
#include "image_input.h"
#include "image_output.h"
#include "agent/tool_runtime.h"
#include "agent/tool_context.h"
#include "util/log.h"
#include "util/error.h"
#include "cJSON.h"
#include "stb_image.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

static int normalize_format(const char *fmt, char *out, size_t out_len)
{
	if (!fmt || !out || out_len == 0)
		return -EINVAL;
	if (strcasecmp(fmt, "png") == 0) {
		strncpy(out, "png", out_len - 1);
	} else if (strcasecmp(fmt, "jpg") == 0 || strcasecmp(fmt, "jpeg") == 0) {
		strncpy(out, "jpg", out_len - 1);
	} else if (strcasecmp(fmt, "bmp") == 0) {
		strncpy(out, "bmp", out_len - 1);
	} else if (strcasecmp(fmt, "tga") == 0) {
		strncpy(out, "tga", out_len - 1);
	} else {
		return -EINVAL;
	}
	out[out_len - 1] = '\0';
	return 0;
}

static int img_convert_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	if (!result)
		return -EINVAL;

	struct tool_context *tctx = user_data;

	cJSON *root = cJSON_Parse(args_json);
	if (!root) {
		(void)tool_result_success_json_text(result, strdup("{\"error\":\"invalid JSON\"}"));
		return -EINVAL;
	}

	cJSON *fp = cJSON_GetObjectItem(root, "file_path");
	cJSON *fmt = cJSON_GetObjectItem(root, "format");
	cJSON *out = cJSON_GetObjectItem(root, "output_path");
	cJSON *q = cJSON_GetObjectItem(root, "quality");

	const char *file_path = cJSON_IsString(fp) ? fp->valuestring : NULL;
	const char *fmt_in = cJSON_IsString(fmt) ? fmt->valuestring : NULL;
	const char *out_path_in = cJSON_IsString(out) ? out->valuestring : NULL;
	if (cJSON_IsNumber(q) &&
	    (!isfinite(q->valuedouble) || q->valuedouble < 1 || q->valuedouble > 100)) {
		cJSON_Delete(root);
		(void)tool_result_error(result, "invalid_arguments",
					"JPEG quality must be between 1 and 100");
		return -EINVAL;
	}
	int quality = cJSON_IsNumber(q) ? (int)q->valuedouble : 90;

	if (!file_path || !fmt_in) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text(
			result, strdup("{\"error\":\"missing 'file_path' or 'format' parameter. "
				       "Usage: img_convert({\\\"file_path\\\": \\\"img.png\\\", "
				       "\\\"format\\\": \\\"jpg\\\"})\"}"));
		return -EINVAL;
	}

	char nfmt[8];
	if (normalize_format(fmt_in, nfmt, sizeof(nfmt)) < 0) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text(
			result, strdup("{\"error\":\"unsupported format (use png/jpg/bmp/tga)\"}"));
		return -EINVAL;
	}

	struct tool_image_input input;
	int input_rc = tool_image_load_input(tctx, file_path, result, &input);

	if (input_rc != 0) {
		cJSON_Delete(root);
		return input_rc;
	}
	int w = input.width, h = input.height, ch = input.channels;
	unsigned char *data = input.pixels;

	char final_path[PATH_MAX];
	struct tool_image_output output = {
		.pixels = data,
		.width = w,
		.height = h,
		.channels = ch,
		.format = nfmt,
		.quality = quality,
	};
	int output_rc = tool_image_write_output(tctx, out_path_in, "img_converted", &output, result,
						final_path, sizeof(final_path));

	stbi_image_free(data);
	if (output_rc != 0) {
		cJSON_Delete(root);
		return output_rc;
	}

	size_t msg_len = strlen(final_path) + strlen(nfmt) + 64;
	char *msg = malloc(msg_len);
	if (!msg) {
		cJSON_Delete(root);
		return -ENOMEM;
	}
	snprintf(msg, msg_len, "image converted: %s (%s, %dx%d)", final_path, nfmt, w, h);
	(void)tool_result_success_json_text(result, msg);
	(void)tool_result_add_image(result, final_path, w, h);
	log_dbg(result && result->runtime ? result->runtime->http.logger : NULL, "img_convert: %s",
		msg);

	cJSON_Delete(root);
	return 0;
}

int img_convert_init(struct tool_registry *reg, struct tool_context *tctx)
{
	if (!reg)
		return -EINVAL;
	return tool_register(
		reg,
		&(struct tool_spec){
			.origin = TOOL_ORIGIN_BUILTIN,
			.flags = TOOL_FLAG_MEDIA,
			.name = "img_convert",
			.description = "Convert an image to another format (png/jpg/bmp/tga). "
				       "Provide file_path, format, optional output_path and "
				       "quality (jpg only).",
			.input_schema =
				"{\"type\":\"object\",\"properties\":"
				"{\"file_path\":{\"type\":\"string\",\"description\":\"Path to the "
				"image file\"},"
				"\"format\":{\"type\":\"string\",\"enum\":[\"png\",\"jpg\","
				"\"jpeg\",\"bmp\",\"tga\"],\"description\":\"Target image "
				"format\"},"
				"\"output_path\":{\"type\":\"string\",\"description\":\"Output "
				"file path (optional)\"},"
				"\"quality\":{\"type\":\"integer\",\"description\":\"JPEG quality "
				"(1-100, optional)\"}},\"required\":[\"file_path\",\"format\"]}",
			.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
			.exec = img_convert_exec,
			.user_data = tctx,
			.user_data_destroy = NULL});
}
