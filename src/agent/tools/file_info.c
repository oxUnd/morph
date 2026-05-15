#include "file_info.h"
#include "util/log.h"
#include "util/file.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

static const char *file_type_str(mode_t mode)
{
	if (S_ISREG(mode)) return "file";
	if (S_ISDIR(mode)) return "dir";
	if (S_ISLNK(mode)) return "symlink";
	if (S_ISFIFO(mode)) return "fifo";
	if (S_ISSOCK(mode)) return "socket";
	if (S_ISBLK(mode)) return "block";
	if (S_ISCHR(mode)) return "char";
	return "other";
}

static int file_info_exec(const char *args_json, char **result_json, void *user_data)
{
	(void)user_data;
	if (!result_json) return -EINVAL;

	cJSON *root = cJSON_Parse(args_json);
	if (!root) {
		*result_json = strdup("{\"error\":\"invalid JSON\"}");
		return -EINVAL;
	}

	cJSON *fp = cJSON_GetObjectItem(root, "file_path");
	const char *file_path = cJSON_IsString(fp) ? fp->valuestring : NULL;
	if (!file_path) {
		cJSON_Delete(root);
		*result_json = strdup(
			"{\"error\":\"missing 'file_path' parameter. "
			"Usage: file_info({\\\"file_path\\\": \\\"path/to/file\\\"})\"}");
		return -EINVAL;
	}

	char *expanded = file_expand_path(file_path);
	const char *resolved = expanded ? expanded : file_path;

	struct stat st;
	if (stat(resolved, &st) != 0) {
		free(expanded);
		cJSON_Delete(root);
		*result_json = strdup("{\"error\":\"path does not exist\"}");
		return -ENOENT;
	}

	cJSON *out = cJSON_CreateObject();
	cJSON_AddStringToObject(out, "file_path", file_path);
	cJSON_AddStringToObject(out, "type", file_type_str(st.st_mode));
	cJSON_AddBoolToObject(out, "exists", 1);

	if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
		cJSON_AddNumberToObject(out, "size", (double)st.st_size);
		cJSON_AddNumberToObject(out, "permissions", (double)(st.st_mode & 0777));

		char perms[16];
		snprintf(perms, sizeof(perms), "%o", (unsigned)(st.st_mode & 0777));
		cJSON_AddStringToObject(out, "perms_octal", perms);

		const char *ext = strrchr(file_path, '.');
		if (ext)
			cJSON_AddStringToObject(out, "extension", ext + 1);
	}

	if (S_ISDIR(st.st_mode)) {
		cJSON_AddNumberToObject(out, "size", (double)st.st_size);
	}

	char timebuf[64];
	strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S",
	         localtime(&st.st_mtime));
	cJSON_AddStringToObject(out, "modified", timebuf);

	char *str = cJSON_PrintUnformatted(out);
	cJSON_Delete(out);
	free(expanded);
	cJSON_Delete(root);
	*result_json = str;
	return 0;
}

int file_info_init(struct tool_registry *reg)
{
	if (!reg) return -EINVAL;
	return tool_register(reg, "file_info",
		"Get file or directory metadata (type, size, permissions, modification time, extension). Provide file_path.",
		"{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"}}}",
		file_info_exec, NULL);
}
