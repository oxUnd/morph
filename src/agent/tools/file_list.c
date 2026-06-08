#include "file_list.h"
#include "agent/tool_context.h"
#include "util/log.h"
#include "util/file.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>

static int name_cmp(const void *a, const void *b)
{
	const cJSON *item_a = *(const cJSON **)a;
	const cJSON *item_b = *(const cJSON **)b;
	cJSON *na = cJSON_GetObjectItem(item_a, "name");
	cJSON *nb = cJSON_GetObjectItem(item_b, "name");
	const char *sa = cJSON_IsString(na) ? na->valuestring : "";
	const char *sb = cJSON_IsString(nb) ? nb->valuestring : "";
	return strcmp(sa, sb);
}

static int file_list_exec(const char *args_json, char **result_json, void *user_data)
{
	struct tool_context *tctx = user_data;
	if (!result_json) return -EINVAL;

	cJSON *root = cJSON_Parse(args_json);
	if (!root) {
		*result_json = strdup("{\"error\":\"invalid JSON\"}");
		return -EINVAL;
	}

	cJSON *dp = cJSON_GetObjectItem(root, "dir_path");
	const char *dir_path = cJSON_IsString(dp) ? dp->valuestring : NULL;
	if (!dir_path) {
		cJSON_Delete(root);
		*result_json = strdup(
			"{\"error\":\"missing 'dir_path' parameter. "
			"Usage: file_list({\\\"dir_path\\\": \\\"path/to/dir\\\"})\"}");
		return -EINVAL;
	}

	char resolved_path[PATH_MAX];
	if (tctx) {
		int rc = tool_context_authorize_path(tctx, TOOL_PATH_LIST,
						     dir_path, resolved_path,
						     sizeof(resolved_path));
		if (rc < 0) {
			cJSON_Delete(root);
			if (rc == -ENOENT)
				*result_json = strdup(
					"{\"error\":\"directory not found\"}");
			else
				*result_json = strdup(
					"{\"error\":\"list path outside workspace: permission denied\"}");
			return rc;
		}
	} else {
		strncpy(resolved_path, dir_path, sizeof(resolved_path) - 1);
		resolved_path[sizeof(resolved_path) - 1] = '\0';
	}

	DIR *d = opendir(resolved_path);
	if (!d) {
		cJSON_Delete(root);
		*result_json = strdup("{\"error\":\"directory not found\"}");
		return -ENOENT;
	}

	cJSON *out = cJSON_CreateObject();
	cJSON_AddStringToObject(out, "dir_path", dir_path);

	cJSON *entries = cJSON_CreateArray();
	cJSON *dirs = cJSON_CreateArray();
	cJSON *files = cJSON_CreateArray();

	struct dirent *entry;
	while ((entry = readdir(d)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;

		char full[PATH_MAX + NAME_MAX + 2];
		snprintf(full, sizeof(full), "%s/%s", resolved_path, entry->d_name);
		struct stat st;
		int is_dir = 0;
		if (stat(full, &st) == 0)
			is_dir = S_ISDIR(st.st_mode);

		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "name", entry->d_name);
		cJSON_AddStringToObject(e, "type", is_dir ? "dir" : "file");

		if (is_dir)
			cJSON_AddItemToArray(dirs, e);
		else
			cJSON_AddItemToArray(files, e);
	}
	closedir(d);

	int dirc = cJSON_GetArraySize(dirs);
	cJSON **darr = malloc(sizeof(cJSON *) * (size_t)(dirc + 1));
	if (!darr) {
		cJSON_Delete(dirs);
		cJSON_Delete(files);
		cJSON_Delete(entries);
		*result_json = strdup("{\"error\":\"out of memory\"}");
		return -ENOMEM;
	}
	for (int i = 0; i < dirc; i++)
		darr[i] = cJSON_GetArrayItem(dirs, i);
	qsort(darr, (size_t)dirc, sizeof(cJSON *), name_cmp);
	cJSON *sorted_dirs = cJSON_CreateArray();
	for (int i = 0; i < dirc; i++)
		cJSON_AddItemToArray(sorted_dirs, cJSON_Duplicate(darr[i], 1));
	free(darr);

	int filec = cJSON_GetArraySize(files);
	cJSON **farr = malloc(sizeof(cJSON *) * (size_t)(filec + 1));
	if (!farr) {
		cJSON_Delete(sorted_dirs);
		cJSON_Delete(files);
		cJSON_Delete(entries);
		*result_json = strdup("{\"error\":\"out of memory\"}");
		return -ENOMEM;
	}
	for (int i = 0; i < filec; i++)
		farr[i] = cJSON_GetArrayItem(files, i);
	qsort(farr, (size_t)filec, sizeof(cJSON *), name_cmp);
	cJSON *sorted_files = cJSON_CreateArray();
	for (int i = 0; i < filec; i++)
		cJSON_AddItemToArray(sorted_files, cJSON_Duplicate(farr[i], 1));
	free(farr);

	for (int i = 0; i < dirc; i++)
		cJSON_AddItemToArray(entries, cJSON_Duplicate(cJSON_GetArrayItem(sorted_dirs, i), 1));
	for (int i = 0; i < filec; i++)
		cJSON_AddItemToArray(entries, cJSON_Duplicate(cJSON_GetArrayItem(sorted_files, i), 1));

	cJSON_AddItemToObject(out, "entries", entries);
	cJSON_Delete(dirs);
	cJSON_Delete(files);
	cJSON_Delete(sorted_dirs);
	cJSON_Delete(sorted_files);

	char *str = cJSON_PrintUnformatted(out);
	cJSON_Delete(out);
	cJSON_Delete(root);
	*result_json = str;
	return 0;
}

int file_list_init(struct tool_registry *reg, struct tool_context *tctx)
{
	if (!reg) return -EINVAL;
	return tool_register(reg, "file_list",
		"List files and directories in a directory. Provide dir_path. Returns sorted entries with name and type (file/dir).",
		"{\"type\":\"object\",\"properties\":{\"dir_path\":{\"type\":\"string\",\"description\":\"Path to the directory to list\"}},\"required\":[\"dir_path\"]}",
		file_list_exec, tctx, NULL);
}
