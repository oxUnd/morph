#include "file.h"
#include "array.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>

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
	char tmp[PATH_MAX];
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

static int file_list_entries(const char *dir, int want_dir,
			     char ***out_names, int *out_count)
{
	if (!dir || !out_names || !out_count)
		return -EINVAL;
	DIR *d = opendir(dir);
	if (!d)
		return -ENOENT;

	morph_array_t list;
	int rc = morph_array_init(&list, 16, sizeof(char *));
	if (rc != 0) {
		closedir(d);
		return rc;
	}

	struct dirent *entry;
	while ((entry = readdir(d)) != NULL) {
		if (want_dir &&
		    (strcmp(entry->d_name, ".") == 0 ||
		     strcmp(entry->d_name, "..") == 0))
			continue;
		if (!want_dir && entry->d_name[0] == '.')
			continue;

		char full[PATH_MAX];
		snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name);
		struct stat st;
		if (stat(full, &st) != 0)
			continue;
		if (want_dir && !S_ISDIR(st.st_mode))
			continue;
		if (!want_dir && !S_ISREG(st.st_mode))
			continue;

		char **slot = morph_array_push(&list);
		if (!slot) {
			char **names = list.elts;
			for (size_t i = 0; i < list.nelts; i++)
				free(names[i]);
			morph_array_cleanup(&list);
			closedir(d);
			return -ENOMEM;
		}
		*slot = strdup(entry->d_name);
		if (!*slot) {
			char **names = list.elts;
			for (size_t i = 0; i + 1 < list.nelts; i++)
				free(names[i]);
			morph_array_cleanup(&list);
			closedir(d);
			return -ENOMEM;
		}
		if (list.nelts > (size_t)INT_MAX) {
			char **names = list.elts;
			for (size_t i = 0; i < list.nelts; i++)
				free(names[i]);
			morph_array_cleanup(&list);
			closedir(d);
			return -EOVERFLOW;
		}
	}
	closedir(d);
	*out_names = list.elts;
	*out_count = (int)list.nelts;
	list.elts = NULL;
	list.nelts = 0;
	list.cap = 0;
	list.size = 0;
	list.heap_alloc = 0;
	return 0;
}

int file_list_dirs(const char *dir, char ***out_names, int *out_count)
{
	return file_list_entries(dir, 1, out_names, out_count);
}

static int name_cmp(const void *a, const void *b)
{
	return strcmp(*(const char **)a, *(const char **)b);
}

int file_list_files(const char *dir, char ***out_names, int *out_count)
{
	int rc = file_list_entries(dir, 0, out_names, out_count);
	if (rc != 0)
		return rc;
	qsort(*out_names, (size_t)*out_count, sizeof(char *), name_cmp);
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

char *file_resolve_path(const char *path)
{
	if (!path)
		return NULL;
	char *expanded = file_expand_path(path);
	if (!expanded)
		return NULL;
	char resolved[PATH_MAX];
	if (realpath(expanded, resolved)) {
		free(expanded);
		return strdup(resolved);
	}
	char tmp[PATH_MAX];
	strncpy(tmp, expanded, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';
	char *slash = strrchr(tmp, '/');
	if (slash && slash != tmp) {
		*slash = '\0';
		char dir_resolved[PATH_MAX];
		if (realpath(tmp, dir_resolved)) {
			free(expanded);
			size_t dlen = strlen(dir_resolved);
			size_t flen = strlen(slash + 1);
			char *result = malloc(dlen + 1 + flen + 1);
			if (!result)
				return NULL;
			snprintf(result, dlen + 1 + flen + 1,
				 "%s/%s", dir_resolved, slash + 1);
			return result;
		}
	}
	return expanded;
}

int path_is_within(const char *path, const char *dir)
{
	if (!path || !dir)
		return 0;
	char *resolved_path = file_resolve_path(path);
	char *resolved_dir = file_resolve_path(dir);
	if (!resolved_path || !resolved_dir) {
		free(resolved_path);
		free(resolved_dir);
		return 0;
	}
	size_t dlen = strlen(resolved_dir);
	int within = 0;
	if (strncmp(resolved_path, resolved_dir, dlen) == 0) {
		if (resolved_path[dlen] == '/' || resolved_path[dlen] == '\0')
			within = 1;
	}
	free(resolved_path);
	free(resolved_dir);
	return within;
}
