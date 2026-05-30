#include "img_resize.h"
#include "agent/tool_context.h"
#include "util/log.h"
#include "util/file.h"
#include "util/error.h"
#include "cJSON.h"
#include "stb_image.h"
#include "stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <time.h>

static const char *out_ext(const char *path)
{
	const char *e = strrchr(path, '.');
	if (!e)
		return "png";
	if (strcasecmp(e, ".jpg") == 0 || strcasecmp(e, ".jpeg") == 0)
		return "jpg";
	if (strcasecmp(e, ".bmp") == 0)
		return "bmp";
	if (strcasecmp(e, ".tga") == 0)
		return "tga";
	return "png";
}

static int write_by_ext(const char *path, const char *ext, int w, int h, int ch,
			const unsigned char *data)
{
	if (strcmp(ext, "png") == 0)
		return stbi_write_png(path, w, h, ch, data, w * ch);
	if (strcmp(ext, "jpg") == 0)
		return stbi_write_jpg(path, w, h, ch, data, 90);
	if (strcmp(ext, "bmp") == 0)
		return stbi_write_bmp(path, w, h, ch, data);
	if (strcmp(ext, "tga") == 0)
		return stbi_write_tga(path, w, h, ch, data);
	return stbi_write_png(path, w, h, ch, data, w * ch);
}

static int img_resize_exec(const char *args_json, char **result_json, void *user_data)
{
	if (!result_json)
		return -EINVAL;

	struct tool_context *tctx = user_data;

	cJSON *root = cJSON_Parse(args_json);
	if (!root) {
		*result_json = strdup("{\"error\":\"invalid JSON\"}");
		return -EINVAL;
	}

	cJSON *fp = cJSON_GetObjectItem(root, "file_path");
	cJSON *cw = cJSON_GetObjectItem(root, "width");
	cJSON *chh = cJSON_GetObjectItem(root, "height");
	cJSON *out = cJSON_GetObjectItem(root, "output_path");

	const char *file_path = cJSON_IsString(fp) ? fp->valuestring : NULL;
	int target_w = cJSON_IsNumber(cw) ? (int)cw->valuedouble : 0;
	int target_h = cJSON_IsNumber(chh) ? (int)chh->valuedouble : 0;
	const char *out_path_in = cJSON_IsString(out) ? out->valuestring : NULL;

	if (!file_path || (target_w <= 0 && target_h <= 0)) {
		cJSON_Delete(root);
		*result_json = strdup(
			"{\"error\":\"missing 'file_path' or 'width'/'height' parameter. "
			"Usage: img_resize({\\\"file_path\\\": \\\"img.png\\\", "
			"\\\"width\\\": 800, \\\"height\\\": 600})\"}");
		return -EINVAL;
	}

	int src_w = 0, src_h = 0, src_ch = 0;
	unsigned char *src = stbi_load(file_path, &src_w, &src_h, &src_ch, 0);
	if (!src) {
		cJSON_Delete(root);
		*result_json = strdup("{\"error\":\"failed to load image\"}");
		MORPH_RETURN(MORPH_ERR_FORMAT);
	}

	/* Maintain aspect ratio if only one dim is given */
	if (target_w <= 0)
		target_w = (int)((double)src_w * target_h / src_h);
	if (target_h <= 0)
		target_h = (int)((double)src_h * target_w / src_w);
	if (target_w <= 0)
		target_w = 1;
	if (target_h <= 0)
		target_h = 1;

	unsigned char *dst = malloc((size_t)target_w * (size_t)target_h * (size_t)src_ch);
	if (!dst) {
		stbi_image_free(src);
		cJSON_Delete(root);
		*result_json = strdup("{\"error\":\"oom\"}");
		return -ENOMEM;
	}

	if (!stbir_resize_uint8_linear(src, src_w, src_h, 0,
				       dst, target_w, target_h, 0,
				       (stbir_pixel_layout)src_ch)) {
		stbi_image_free(src);
		free(dst);
		cJSON_Delete(root);
		*result_json = strdup("{\"error\":\"resize failed\"}");
		MORPH_RETURN(MORPH_ERR_PROCESSING);
	}
	stbi_image_free(src);

	char final_path[1024];
	if (out_path_in && *out_path_in) {
		if (tctx && tool_context_check_write_path(tctx, out_path_in) < 0) {
			cJSON_Delete(root);
			*result_json = strdup(
				"{\"error\":\"write path outside output directory: permission denied\"}");
			MORPH_RETURN(-EPERM);
		}
		strncpy(final_path, out_path_in, sizeof(final_path) - 1);
		final_path[sizeof(final_path) - 1] = '\0';
	} else {
		const char *odir = tctx ? tool_context_output_dir(tctx) : NULL;
		char *out_dir;
		if (odir)
			out_dir = file_expand_path(odir);
		else
			out_dir = file_expand_path("~/.morph/output");
		if (!out_dir) {
			stbi_image_free(dst);
			stbi_image_free(src);
			cJSON_Delete(root);
			*result_json = strdup("{\"error\":\"failed to expand output path\"}");
			return -ENOMEM;
		}
		file_ensure_dir(out_dir);
		const char *ext = out_ext(file_path);
		snprintf(final_path, sizeof(final_path),
			 "%s/img_resized_%lld.%s",
			 out_dir, (long long)time(NULL), ext);
		free(out_dir);
	}

	const char *ext = out_ext(final_path);
	int wrc = write_by_ext(final_path, ext, target_w, target_h, src_ch, dst);
	free(dst);

	if (!wrc) {
		cJSON_Delete(root);
		*result_json = strdup("{\"error\":\"failed to write output\"}");
		return -EIO;
	}

	char buf[1280];
	snprintf(buf, sizeof(buf),
		 "image resized: %s (%dx%d)", final_path, target_w, target_h);
	*result_json = strdup(buf);
	log_dbg("img_resize: %s", buf);

	cJSON_Delete(root);
	return 0;
}

int img_resize_init(struct tool_registry *reg, struct tool_context *tctx)
{
	if (!reg)
		return -EINVAL;
	return tool_register(reg, "img_resize",
		"Resize an image to the given width/height. "
		"Provide file_path, width, height, optional output_path. "
		"If only one of width/height is given, the other is computed "
		"to preserve aspect ratio.",
		"{\"type\":\"object\",\"properties\":"
		"{\"file_path\":{\"type\":\"string\",\"description\":\"Path to the image file\"},"
		"\"width\":{\"type\":\"integer\",\"description\":\"Target width in pixels\"},"
		"\"height\":{\"type\":\"integer\",\"description\":\"Target height in pixels\"},"
		"\"output_path\":{\"type\":\"string\",\"description\":\"Output file path (optional)\"}},\"required\":[\"file_path\"]}",
		img_resize_exec, tctx, NULL);
}
