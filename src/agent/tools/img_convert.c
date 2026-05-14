#include "img_convert.h"
#include "util/log.h"
#include "util/file.h"
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

static int img_convert_exec(const char *args_json, char **result_json, void *user_data)
{
	(void)user_data;
	if (!result_json)
		return -EINVAL;

	cJSON *root = cJSON_Parse(args_json);
	if (!root) {
		*result_json = strdup("{\"error\":\"invalid JSON\"}");
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
		*result_json = strdup("{\"error\":\"missing file_path or format\"}");
		return -EINVAL;
	}

	char nfmt[8];
	if (normalize_format(fmt_in, nfmt, sizeof(nfmt)) < 0) {
		cJSON_Delete(root);
		*result_json = strdup("{\"error\":\"unsupported format (use png/jpg/bmp/tga)\"}");
		return -EINVAL;
	}

	int w = 0, h = 0, ch = 0;
	unsigned char *data = stbi_load(file_path, &w, &h, &ch, 0);
	if (!data) {
		cJSON_Delete(root);
		*result_json = strdup("{\"error\":\"failed to load image\"}");
		return -EIO;
	}

	char final_path[1024];
	if (out_path_in && *out_path_in) {
		strncpy(final_path, out_path_in, sizeof(final_path) - 1);
		final_path[sizeof(final_path) - 1] = '\0';
	} else {
		char *out_dir = file_expand_path("~/.morph/output");
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
		*result_json = strdup("{\"error\":\"failed to write output\"}");
		return -EIO;
	}

	char buf[1280];
	snprintf(buf, sizeof(buf),
		 "image converted: %s (%s, %dx%d)",
		 final_path, nfmt, w, h);
	*result_json = strdup(buf);
	log_info("img_convert: %s", buf);

	cJSON_Delete(root);
	return 0;
}

int img_convert_init(struct tool_registry *reg)
{
	if (!reg)
		return -EINVAL;
	return tool_register(reg, "img_convert",
		"Convert an image to another format (png/jpg/bmp/tga). "
		"Provide file_path, format, optional output_path and quality (jpg only).",
		"{\"type\":\"object\",\"properties\":"
		"{\"file_path\":{\"type\":\"string\"},"
		"\"format\":{\"type\":\"string\",\"enum\":[\"png\",\"jpg\",\"jpeg\",\"bmp\",\"tga\"]},"
		"\"output_path\":{\"type\":\"string\"},"
		"\"quality\":{\"type\":\"integer\"}}}",
		img_convert_exec, NULL);
}
