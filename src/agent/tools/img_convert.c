#include "img_convert.h"
#include "agent/tool_context.h"
#include "util/log.h"
#include "util/file.h"
#include "util/error.h"
#include "cJSON.h"
#include "stb_image.h"
#include "stb_image_write.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <time.h>

static int normalize_format(const char *fmt, char *out, size_t out_len)
{
	if (!fmt || !out || out_len == 0)
		return -EINVAL;
	if (strcasecmp(fmt, "png") == 0) {
		strncpy(out, "png", out_len - 1);
	} else if (strcasecmp(fmt, "jpg") == 0 ||
		   strcasecmp(fmt, "jpeg") == 0) {
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

static int write_by_format(const char *path, const char *fmt, int w, int h,
			   int ch, const unsigned char *data, int quality)
{
	if (strcmp(fmt, "png") == 0)
		return stbi_write_png(path, w, h, ch, data, w * ch);
	if (strcmp(fmt, "jpg") == 0) {
		/* JPEG only supports 1/3 channels — drop alpha if present */
		if (ch == 4) {
			unsigned char *tmp = malloc((size_t)w * (size_t)h * 3);
			if (!tmp)
				return 0;
			for (int i = 0; i < w * h; i++) {
				tmp[i * 3 + 0] = data[i * 4 + 0];
				tmp[i * 3 + 1] = data[i * 4 + 1];
				tmp[i * 3 + 2] = data[i * 4 + 2];
			}
			int rc = stbi_write_jpg(path, w, h, 3, tmp, quality);
			free(tmp);
			return rc;
		}
		return stbi_write_jpg(path, w, h, ch, data, quality);
	}
	if (strcmp(fmt, "bmp") == 0)
		return stbi_write_bmp(path, w, h, ch, data);
	if (strcmp(fmt, "tga") == 0)
		return stbi_write_tga(path, w, h, ch, data);
	return 0;
}

static int img_convert_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	if (!result)
		return -EINVAL;

	struct tool_context *tctx = user_data;

	cJSON *root = cJSON_Parse(args_json);
	if (!root) {
		(void)tool_result_take_text(result, strdup("{\"error\":\"invalid JSON\"}"));
		return -EINVAL;
	}

	cJSON *fp = cJSON_GetObjectItem(root, "file_path");
	cJSON *fmt = cJSON_GetObjectItem(root, "format");
	cJSON *out = cJSON_GetObjectItem(root, "output_path");
	cJSON *q = cJSON_GetObjectItem(root, "quality");

	const char *file_path = cJSON_IsString(fp) ? fp->valuestring : NULL;
	const char *fmt_in = cJSON_IsString(fmt) ? fmt->valuestring : NULL;
	const char *out_path_in = cJSON_IsString(out) ? out->valuestring : NULL;
	int quality = cJSON_IsNumber(q) ? (int)q->valuedouble : 90;

	if (!file_path || !fmt_in) {
		cJSON_Delete(root);
		(void)tool_result_take_text(result, strdup(
			"{\"error\":\"missing 'file_path' or 'format' parameter. "
			"Usage: img_convert({\\\"file_path\\\": \\\"img.png\\\", "
			"\\\"format\\\": \\\"jpg\\\"})\"}"));
		return -EINVAL;
	}

	char nfmt[8];
	if (normalize_format(fmt_in, nfmt, sizeof(nfmt)) < 0) {
		cJSON_Delete(root);
		(void)tool_result_take_text(result, strdup("{\"error\":\"unsupported format (use png/jpg/bmp/tga)\"}"));
		return -EINVAL;
	}

	char resolved_input[PATH_MAX];
	if (tctx) {
		int rc = tool_context_authorize_path(tctx, TOOL_PATH_READ,
						     file_path, resolved_input,
						     sizeof(resolved_input));
		if (rc < 0) {
			cJSON_Delete(root);
			if (rc == -ENOENT)
				(void)tool_result_take_text(result, strdup(
					"{\"error\":\"failed to load image\"}"));
			else
				(void)tool_result_take_text(result, strdup(
					"{\"error\":\"read path outside workspace: permission denied\"}"));
			return rc;
		}
	} else {
		strncpy(resolved_input, file_path, sizeof(resolved_input) - 1);
		resolved_input[sizeof(resolved_input) - 1] = '\0';
	}

	int w = 0, h = 0, ch = 0;
	unsigned char *data = stbi_load(resolved_input, &w, &h, &ch, 0);
	if (!data) {
		cJSON_Delete(root);
		(void)tool_result_take_text(result, strdup("{\"error\":\"failed to load image\"}"));
		MORPH_RETURN(MORPH_ERR_FORMAT);
	}

	char final_path[PATH_MAX];
	if (out_path_in && *out_path_in) {
		if (tctx) {
			int rc = tool_context_authorize_path(
				tctx, TOOL_PATH_WRITE, out_path_in,
				final_path, sizeof(final_path));
			if (rc < 0) {
				stbi_image_free(data);
				cJSON_Delete(root);
				(void)tool_result_take_text(result, strdup(
					"{\"error\":\"write path outside output directory: permission denied\"}"));
				return rc;
			}
		} else {
			strncpy(final_path, out_path_in, sizeof(final_path) - 1);
			final_path[sizeof(final_path) - 1] = '\0';
		}
	} else {
		const char *odir = tctx ? tool_context_output_dir(tctx) : NULL;
		char *out_dir;
		if (odir)
			out_dir = file_expand_path(odir);
		else
			out_dir = file_expand_path("~/.morph/output");
		if (!out_dir) {
			stbi_image_free(data);
			cJSON_Delete(root);
			(void)tool_result_take_text(result, strdup("{\"error\":\"failed to expand output path\"}"));
			return -ENOMEM;
		}
		file_ensure_dir(out_dir);
		snprintf(final_path, sizeof(final_path),
			 "%s/img_converted_%lld.%s",
			 out_dir, (long long)time(NULL), nfmt);
		free(out_dir);
	}

	int wrc = write_by_format(final_path, nfmt, w, h, ch, data, quality);
	stbi_image_free(data);

	if (!wrc) {
		cJSON_Delete(root);
		(void)tool_result_take_text(result, strdup("{\"error\":\"failed to write output\"}"));
		return -EIO;
	}

	size_t msg_len = strlen(final_path) + strlen(nfmt) + 64;
	char *msg = malloc(msg_len);
	if (!msg) {
		cJSON_Delete(root);
		return -ENOMEM;
	}
	snprintf(msg, msg_len, "image converted: %s (%s, %dx%d)",
		 final_path, nfmt, w, h);
	(void)tool_result_take_text(result, msg);
	(void)tool_result_add_image(result, final_path, w, h);
	log_dbg("img_convert: %s", msg);

	cJSON_Delete(root);
	return 0;
}

int img_convert_init(struct tool_registry *reg, struct tool_context *tctx)
{
	if (!reg)
		return -EINVAL;
	return tool_register(TOOL_ORIGIN_BUILTIN, reg, "img_convert",
		"Convert an image to another format (png/jpg/bmp/tga). "
		"Provide file_path, format, optional output_path and quality (jpg only).",
		"{\"type\":\"object\",\"properties\":"
		"{\"file_path\":{\"type\":\"string\",\"description\":\"Path to the image file\"},"
		"\"format\":{\"type\":\"string\",\"enum\":[\"png\",\"jpg\",\"jpeg\",\"bmp\",\"tga\"],\"description\":\"Target image format\"},"
		"\"output_path\":{\"type\":\"string\",\"description\":\"Output file path (optional)\"},"
		"\"quality\":{\"type\":\"integer\",\"description\":\"JPEG quality (1-100, optional)\"}},\"required\":[\"file_path\",\"format\"]}",
		img_convert_exec, tctx, NULL);
}
