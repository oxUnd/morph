#include "file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

char *file_read_all(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0) {
		fclose(f);
		return NULL;
	}
	char *buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	size_t rd = fread(buf, 1, (size_t)sz, f);
	buf[rd] = '\0';
	fclose(f);
	if (out_len)
		*out_len = rd;
	return buf;
}

int file_write_all(const char *path, const char *data, size_t len)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -errno;
	if (len > 0 && fwrite(data, 1, len, f) != len) {
		fclose(f);
		return -EIO;
	}
	fclose(f);
	return 0;
}

int file_ensure_dir(const char *path)
{
	char tmp[4096];
	strncpy(tmp, path, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';
	size_t len = strlen(tmp);
	if (len > 0 && tmp[len - 1] == '/')
		tmp[len - 1] = '\0';
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
				return -errno;
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		return -errno;
	return 0;
}

int file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

char *file_expand_path(const char *path)
{
	if (path[0] == '~') {
		const char *home = getenv("HOME");
		if (!home)
			home = "/tmp";
		size_t home_len = strlen(home);
		size_t path_len = strlen(path);
		char *result = malloc(home_len + path_len);
		if (!result)
			return NULL;
		memcpy(result, home, home_len);
		strcpy(result + home_len, path + 1);
		return result;
	}
	return strdup(path);
}

int file_list_dirs(const char *dir, char ***out_names, int *out_count)
{
	if (!dir || !out_names || !out_count)
		return -EINVAL;
	DIR *d = opendir(dir);
	if (!d)
		return -ENOENT;
	int cap = 16;
	int n = 0;
	char **list = malloc(sizeof(char *) * (size_t)cap);
	if (!list) {
		closedir(d);
		return -ENOMEM;
	}
	struct dirent *entry;
	while ((entry = readdir(d)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		char full[4096];
		snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name);
		struct stat st;
		if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode))
			continue;
		if (n >= cap) {
			cap *= 2;
			char **new_list = realloc(list, sizeof(char *) * (size_t)cap);
			if (!new_list) {
				for (int i = 0; i < n; i++)
					free(list[i]);
				free(list);
				closedir(d);
				return -ENOMEM;
			}
			list = new_list;
		}
		list[n] = strdup(entry->d_name);
		if (!list[n]) {
			for (int i = 0; i < n; i++)
				free(list[i]);
			free(list);
			closedir(d);
			return -ENOMEM;
		}
		n++;
	}
	closedir(d);
	*out_names = list;
	*out_count = n;
	return 0;
}

void file_free_list(char **list, int count)
{
	if (!list)
		return;
	for (int i = 0; i < count; i++)
		free(list[i]);
	free(list);
}