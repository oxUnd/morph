#include "img_info.h"
#include "agent/tool_context.h"
#include "util/log.h"
#include "util/error.h"
#include "util/json.h"
#include "stb_image.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

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

static int img_info_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct tool_context *tctx = user_data;
	if (!result) return -EINVAL;

	struct json *root = json_parse_prefix(args_json, NULL, NULL);
	if (!root) {
		(void)tool_result_success_json_text(result, strdup("{\"error\":\"invalid JSON\"}"));
		return -EINVAL;
	}
	struct json *f = json_member_ci(root, "file_path");
	const char *file_path = json_is_string_value(f) ? json_string_value(f) : NULL;
	if (!file_path) {
		json_free(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"missing 'file_path' parameter. "
			"Usage: img_info({\\\"file_path\\\": \\\"img.png\\\"})\"}"));
		return -EINVAL;
	}

	char resolved_path[PATH_MAX];
	if (tctx) {
		int rc = tool_context_authorize_path(tctx, TOOL_PATH_READ,
						     file_path, resolved_path,
						     sizeof(resolved_path));
		if (rc < 0) {
			json_free(root);
			if (rc == -ENOENT)
				(void)tool_result_success_json_text(result, strdup(
					"{\"error\":\"not a valid image\"}"));
			else
				(void)tool_result_success_json_text(result, strdup(
					"{\"error\":\"read path outside workspace: permission denied\"}"));
			return rc;
		}
	} else {
		strncpy(resolved_path, file_path, sizeof(resolved_path) - 1);
		resolved_path[sizeof(resolved_path) - 1] = '\0';
	}

	int w = 0, h = 0, ch = 0;
	if (!stbi_info(resolved_path, &w, &h, &ch)) {
		json_free(root);
		(void)tool_result_success_json_text(result, strdup("{\"error\":\"not a valid image\"}"));
		return MORPH_ERR_FORMAT;
	}

	struct json *out = json_object();
	json_add_string(out, "file_path", file_path);
	json_add_number(out, "width", w);
	json_add_number(out, "height", h);
	json_add_number(out, "channels", ch);
	json_add_string(out, "format", fmt_name(file_path));
	char *str = json_stringify(out);
	json_free(out);
	json_free(root);
	(void)tool_result_success_json_text(result, str);
	return 0;
}

int img_info_init(struct tool_registry *reg, struct tool_context *tctx)
{
	if (!reg) return -EINVAL;
	return tool_register(reg, &(struct tool_spec){
		.origin = TOOL_ORIGIN_BUILTIN,
		.flags = TOOL_FLAG_READONLY,
		.name = "img_info",
		.description = "Get image metadata (dimensions, format, color channels). "
			"Provide file_path.",
		.input_schema = "{\"type\":\"object\",\"properties\":{\"file_path\":{"
			"\"type\":\"string\",\"description\":\"Path to the image file\"}},"
			"\"required\":[\"file_path\"]}",
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.exec = img_info_exec,
		.user_data = tctx,
	});
}
