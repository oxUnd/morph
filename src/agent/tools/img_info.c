#include "img_info.h"
#include "util/log.h"
#include "cJSON.h"
#include "stb_image.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *fmt_name(const char *path)
{
	const char *e = strrchr(path, '.');
	if (!e) return "unknown";
	if (strcasecmp(e, ".png") == 0) return "png";
	if (strcasecmp(e, ".jpg") == 0 || strcasecmp(e, ".jpeg") == 0) return "jpeg";
	if (strcasecmp(e, ".gif") == 0) return "gif";
	if (strcasecmp(e, ".webp") == 0) return "webp";
	if (strcasecmp(e, ".bmp") == 0) return "bmp";
	return "unknown";
}

static int img_info_exec(const char *args_json, char **result_json, void *user_data)
{
	(void)user_data;
	if (!result_json) return -EINVAL;

	cJSON *root = cJSON_Parse(args_json);
	if (!root) {
		*result_json = strdup("{\"error\":\"invalid JSON\"}");
		return -EINVAL;
	}
	cJSON *f = cJSON_GetObjectItem(root, "file_path");
	const char *file_path = cJSON_IsString(f) ? f->valuestring : NULL;
	if (!file_path) {
		cJSON_Delete(root);
		*result_json = strdup(
			"{\"error\":\"missing 'file_path' parameter. "
			"Usage: img_info({\\\"file_path\\\": \\\"img.png\\\"})\"}");
		return -EINVAL;
	}

	int w = 0, h = 0, ch = 0;
	if (!stbi_info(file_path, &w, &h, &ch)) {
		cJSON_Delete(root);
		*result_json = strdup("{\"error\":\"not a valid image\"}");
		return -EIO;
	}

	cJSON *out = cJSON_CreateObject();
	cJSON_AddStringToObject(out, "file_path", file_path);
	cJSON_AddNumberToObject(out, "width", w);
	cJSON_AddNumberToObject(out, "height", h);
	cJSON_AddNumberToObject(out, "channels", ch);
	cJSON_AddStringToObject(out, "format", fmt_name(file_path));
	char *str = cJSON_PrintUnformatted(out);
	cJSON_Delete(out);
	cJSON_Delete(root);
	*result_json = str;
	return 0;
}

int img_info_init(struct tool_registry *reg)
{
	if (!reg) return -EINVAL;
	return tool_register(reg, "img_info",
		"Get image metadata (dimensions, format, color channels). Provide file_path.",
		"{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"}}}",
		img_info_exec, NULL);
}