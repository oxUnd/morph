#include "file.h"
#include "array.h"
#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
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
		MORPH_RETURN_ERRNO();
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
				MORPH_RETURN_ERRNO();
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		MORPH_RETURN_ERRNO();
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

int file_path_is_absolute(const char *path)
{
	return path && path[0] == '/';
}

int file_path_full(char *dst, size_t dst_size,
		   const char *prefix, const char *path)
{
	if (!dst || dst_size == 0 || !path)
		MORPH_RETURN(-EINVAL);

	if (file_path_is_absolute(path)) {
		size_t len = strlen(path);
		if (len + 1 > dst_size)
			MORPH_RETURN(-ENAMETOOLONG);
		memcpy(dst, path, len + 1);
		return 0;
	}

	if (!prefix)
		MORPH_RETURN(-EINVAL);
	return file_path_join(dst, dst_size, prefix, path);
}

char *file_path_full_alloc(const char *prefix, const char *path)
{
	if (!path)
		return NULL;
	if (file_path_is_absolute(path))
		return strdup(path);
	return file_path_join_alloc(prefix, path);
}

int file_path_join(char *dst, size_t dst_size,
		   const char *dir, const char *name)
{
	if (!dst || dst_size == 0 || !dir || !name)
		MORPH_RETURN(-EINVAL);

	size_t dir_len = strlen(dir);
	size_t name_len = strlen(name);
	int need_sep = dir_len > 0 && dir[dir_len - 1] != '/' &&
		name_len > 0 && name[0] != '/';
	int skip_sep = dir_len > 0 && dir[dir_len - 1] == '/' &&
		name_len > 0 && name[0] == '/';
	size_t name_off = skip_sep ? 1u : 0u;
	size_t out_name_len = name_len - name_off;
	if (dir_len > SIZE_MAX - (need_sep ? 1u : 0u) ||
	    dir_len + (need_sep ? 1u : 0u) > SIZE_MAX - out_name_len)
		MORPH_RETURN(-ENAMETOOLONG);
	size_t total = dir_len + (need_sep ? 1u : 0u) + out_name_len;

	if (total == SIZE_MAX)
		MORPH_RETURN(-ENAMETOOLONG);
	if (total + 1 > dst_size)
		MORPH_RETURN(-ENAMETOOLONG);

	memcpy(dst, dir, dir_len);
	size_t pos = dir_len;
	if (need_sep)
		dst[pos++] = '/';
	if (out_name_len > 0) {
		memcpy(dst + pos, name + name_off, out_name_len);
		pos += out_name_len;
	}
	dst[pos] = '\0';
	return 0;
}

char *file_path_join_alloc(const char *dir, const char *name)
{
	if (!dir || !name)
		return NULL;

	size_t dir_len = strlen(dir);
	size_t name_len = strlen(name);
	int need_sep = dir_len > 0 && dir[dir_len - 1] != '/' &&
		name_len > 0 && name[0] != '/';
	int skip_sep = dir_len > 0 && dir[dir_len - 1] == '/' &&
		name_len > 0 && name[0] == '/';
	size_t name_off = skip_sep ? 1u : 0u;
	size_t out_name_len = name_len - name_off;
	if (dir_len > SIZE_MAX - (need_sep ? 1u : 0u) ||
	    dir_len + (need_sep ? 1u : 0u) > SIZE_MAX - out_name_len)
		return NULL;
	size_t total = dir_len + (need_sep ? 1u : 0u) + out_name_len;
	if (total == SIZE_MAX)
		return NULL;

	char *result = malloc(total + 1);
	if (!result)
		return NULL;
	if (file_path_join(result, total + 1, dir, name) != 0) {
		free(result);
		return NULL;
	}
	return result;
}

int file_path_append(char *dst, size_t dst_size, const char *suffix)
{
	if (!dst || dst_size == 0 || !suffix)
		MORPH_RETURN(-EINVAL);

	size_t len = strlen(dst);
	size_t suffix_len = strlen(suffix);
	if (len > SIZE_MAX - suffix_len)
		MORPH_RETURN(-ENAMETOOLONG);
	size_t total = len + suffix_len;
	if (total == SIZE_MAX || total + 1 > dst_size)
		MORPH_RETURN(-ENAMETOOLONG);

	memcpy(dst + len, suffix, suffix_len + 1);
	return 0;
}

char *file_path_append_alloc(const char *path, const char *suffix)
{
	if (!path || !suffix)
		return NULL;

	size_t len = strlen(path);
	size_t suffix_len = strlen(suffix);
	if (len > SIZE_MAX - suffix_len)
		return NULL;
	size_t total = len + suffix_len;
	if (total == SIZE_MAX)
		return NULL;

	char *result = malloc(total + 1);
	if (!result)
		return NULL;
	memcpy(result, path, len);
	memcpy(result + len, suffix, suffix_len + 1);
	return result;
}

int file_path_join_append(char *dst, size_t dst_size,
			  const char *dir, const char *name,
			  const char *suffix)
{
	int rc = file_path_join(dst, dst_size, dir, name);
	if (rc < 0)
		return rc;
	return file_path_append(dst, dst_size, suffix);
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

		char full[PATH_MAX + NAME_MAX + 2];
		if (file_path_join(full, sizeof(full), dir, entry->d_name) != 0)
			continue;
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
			char *result = file_path_join_alloc(dir_resolved,
							    slash + 1);
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
