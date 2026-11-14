#include "resources.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/file.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int find_under_root(char *dst, size_t dst_size,
			   const char *root, const char *relative_path);

static int copy_path(char *dst, size_t dst_size, const char *path)
{
	size_t len;

	if (!dst || dst_size == 0u || !path)
		return NB_ERR_INVAL;
	len = strlen(path);
	if (len + 1u > dst_size)
		return NB_ERR_NAME_TOO_LONG;
	memcpy(dst, path, len + 1u);
	return 0;
}

static int relative_path_valid(const char *path)
{
	const char *component = path;

	if (!path || !path[0] || nb_path_is_absolute(path))
		return 0;
	for (const char *p = path;; p++) {
		if (*p != '/' && *p != '\0')
			continue;
		size_t len = (size_t)(p - component);
		if ((len == 1u && component[0] == '.') ||
		    (len == 2u && component[0] == '.' &&
		     component[1] == '.'))
			return 0;
		if (*p == '\0')
			break;
		component = p + 1;
	}
	return 1;
}

static int executable_dir(char *dst, size_t dst_size)
{
	nb_buf executable NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf directory NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_status status = nb_path_executable(&executable, NULL);
	if (status != NB_OK)
		return status;
	status = nb_path_dirname(&directory, nb_buf_cstr(&executable));
	if (status != NB_OK)
		return status;
	return copy_path(dst, dst_size, nb_buf_cstr(&directory));
}

int morph_executable_find(char *dst, size_t dst_size, const char *name)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;

	char dir[PATH_MAX];
	char path[PATH_MAX];
	int rc;

	if (!dst || dst_size == 0u || !name || !name[0] ||
	    strchr(name, '/'))
		return NB_ERR_INVAL;
	rc = executable_dir(dir, sizeof(dir));
	if (rc != 0)
		return rc;
	rc = ((file_status_1 = nb_path_concat(&file_value_1, dir, name)) != NB_OK ? file_status_1
	      : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), path, sizeof(path))) ==
			      NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_1);
	if (rc != 0)
		return rc;
	if (access(path, X_OK) != 0)
		return NB_ERR_NOT_FOUND;
	rc = copy_path(dst, dst_size, path);
	if (rc != 0)
		return rc;
	return 0;
}

static int find_under_root(char *dst, size_t dst_size,
			   const char *root, const char *relative_path)
{
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_2 = NB_OK;

	char path[PATH_MAX];
	nb_buf resolved NB_CLEANUP(nb_buf_destroy) = {0};
	int rc;

	if (!root || !root[0])
		return NB_ERR_NOT_FOUND;
	rc = ((file_status_2 = nb_path_concat(&file_value_2, root, relative_path)) != NB_OK
		      ? file_status_2
	      : (file_status_2 = nb_str_copy_to(nb_buf_str(&file_value_2), path, sizeof(path))) ==
			      NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_2);
	if (rc != 0)
		return rc;
	if (nb_path_canonical(&resolved, path, NULL) != NB_OK)
		return NB_ERR_NOT_FOUND;
	if (!nb_path_is_within(nb_buf_cstr(&resolved), root, true))
		return NB_ERR_PERMISSION;
	rc = copy_path(dst, dst_size, nb_buf_cstr(&resolved));
	if (rc != 0)
		return rc;
	return 0;
}

static int find_installed(char *dst, size_t dst_size,
			  const char *relative_path)
{
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_3 = NB_OK;

	char dir[PATH_MAX];
	char root[PATH_MAX];
	int rc;

	rc = executable_dir(dir, sizeof(dir));
	if (rc != 0)
		return rc;
	rc = ((file_status_3 = nb_path_concat(&file_value_3, dir, "../share/morph")) != NB_OK
		      ? file_status_3
	      : (file_status_3 = nb_str_copy_to(nb_buf_str(&file_value_3), root, sizeof(root))) ==
			      NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_3);
	if (rc != 0)
		return rc;
	rc = find_under_root(dst, dst_size, root, relative_path);
	if (rc != 0)
		return rc;
	return 0;
}

int morph_data_find(char *dst, size_t dst_size, const char *relative_path)
{
	int rc;

	if (!dst || dst_size == 0u || !relative_path_valid(relative_path))
		return NB_ERR_INVAL;
	rc = find_installed(dst, dst_size, relative_path);
	if (rc != 0)
		return rc;
	return 0;
}

char *morph_data_find_alloc(const char *relative_path)
{
	char path[PATH_MAX];

	if (morph_data_find(path, sizeof(path), relative_path) != 0)
		return NULL;
	return strdup(path);
}
