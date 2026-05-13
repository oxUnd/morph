#include "file_list.h"
#include "util/log.h"
#include "util/file.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

static int name_cmp(const void *a, const void *b)
{
	return strcmp((*(const cJSON **)a)->valuestring,
	              (*(const cJSON **)b)->valuestring);
}

static int file_list_exec(const char *args_json, char **result_json, void *user_data)
{
	(void)user_data;
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
		*result_json = strdup("{\"error\":\"missing dir_path\"}");
		return -EINVAL;
	}

	char *expanded = file_expand_path(dir_path);
	const char *resolved = expanded ? expanded : dir_path;

	DIR *d = opendir(resolved);
	if (!d) {
		free(expanded);
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

		char full[4096];
		snprintf(full, sizeof(full), "%s/%s", resolved, entry->d_name);
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
	free(expanded);

	int dirc = cJSON_GetArraySize(dirs);
	cJSON **darr = malloc(sizeof(cJSON *) * (size_t)(dirc + 1));
	for (int i = 0; i < dirc; i++)
		darr[i] = cJSON_GetArrayItem(dirs, i);
	qsort(darr, (size_t)dirc, sizeof(cJSON *), name_cmp);
	cJSON *sorted_dirs = cJSON_CreateArray();
	for (int i = 0; i < dirc; i++)
		cJSON_AddItemToArray(sorted_dirs, cJSON_Duplicate(darr[i], 1));
	free(darr);

	int filec = cJSON_GetArraySize(files);
	cJSON **farr = malloc(sizeof(cJSON *) * (size_t)(filec + 1));
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

int file_list_init(struct tool_registry *reg)
{
	if (!reg) return -EINVAL;
	return tool_register(reg, "file_list",
		"List files and directories in a directory. Provide dir_path. Returns sorted entries with name and type (file/dir).",
		"{\"type\":\"object\",\"properties\":{\"dir_path\":{\"type\":\"string\"}}}",
		file_list_exec, NULL);
}
