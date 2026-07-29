#include "data.h"
#include "error.h"
#include "file.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static int find_under_root(char *dst, size_t dst_size,
			   const char *root, const char *relative_path);

static int copy_path(char *dst, size_t dst_size, const char *path)
{
	size_t len;

	if (!dst || dst_size == 0u || !path)
		MORPH_RETURN(-EINVAL);
	len = strlen(path);
	if (len + 1u > dst_size)
		MORPH_RETURN(-ENAMETOOLONG);
	memcpy(dst, path, len + 1u);
	return 0;
}

static int relative_path_valid(const char *path)
{
	const char *component = path;

	if (!path || !path[0] || file_path_is_absolute(path))
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
	char path[PATH_MAX];
	char resolved[PATH_MAX];
	char *slash;
	int rc;

#ifdef __APPLE__
	uint32_t path_size = (uint32_t)sizeof(path);

	if (_NSGetExecutablePath(path, &path_size) != 0)
		MORPH_RETURN(-ENAMETOOLONG);
#elif defined(__linux__)
	ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1u);

	if (len < 0)
		MORPH_RETURN_ERRNO();
	path[len] = '\0';
#else
	MORPH_RETURN(-ENOTSUP);
#endif

	if (!realpath(path, resolved))
		MORPH_RETURN_ERRNO();
	slash = strrchr(resolved, '/');
	if (!slash)
		MORPH_RETURN(-ENOENT);
	if (slash == resolved)
		slash[1] = '\0';
	else
		*slash = '\0';
	rc = copy_path(dst, dst_size, resolved);
	if (rc != 0)
		MORPH_RETURN(rc);
	return 0;
}

int morph_executable_find(char *dst, size_t dst_size, const char *name)
{
	char dir[PATH_MAX];
	char path[PATH_MAX];
	int rc;

	if (!dst || dst_size == 0u || !name || !name[0] ||
	    strchr(name, '/'))
		MORPH_RETURN(-EINVAL);
	rc = executable_dir(dir, sizeof(dir));
	if (rc != 0)
		MORPH_RETURN(rc);
	rc = file_path_join(path, sizeof(path), dir, name);
	if (rc != 0)
		MORPH_RETURN(rc);
	if (access(path, X_OK) != 0)
		MORPH_RETURN(-ENOENT);
	rc = copy_path(dst, dst_size, path);
	if (rc != 0)
		MORPH_RETURN(rc);
	return 0;
}

static int find_under_root(char *dst, size_t dst_size,
			   const char *root, const char *relative_path)
{
	char path[PATH_MAX];
	char resolved[PATH_MAX];
	int rc;

	if (!root || !root[0])
		MORPH_RETURN(-ENOENT);
	rc = file_path_join(path, sizeof(path), root, relative_path);
	if (rc != 0)
		MORPH_RETURN(rc);
	if (!realpath(path, resolved))
		MORPH_RETURN(-ENOENT);
	if (!path_is_within(resolved, root))
		MORPH_RETURN(-EPERM);
	rc = copy_path(dst, dst_size, resolved);
	if (rc != 0)
		MORPH_RETURN(rc);
	return 0;
}

static int find_installed(char *dst, size_t dst_size,
			  const char *relative_path)
{
	char dir[PATH_MAX];
	char root[PATH_MAX];
	int rc;

	rc = executable_dir(dir, sizeof(dir));
	if (rc != 0)
		MORPH_RETURN(rc);
	rc = file_path_join(root, sizeof(root), dir, "../share/morph");
	if (rc != 0)
		MORPH_RETURN(rc);
	rc = find_under_root(dst, dst_size, root, relative_path);
	if (rc != 0)
		MORPH_RETURN(rc);
	return 0;
}

int morph_data_find(char *dst, size_t dst_size, const char *relative_path)
{
	int rc;

	if (!dst || dst_size == 0u || !relative_path_valid(relative_path))
		MORPH_RETURN(-EINVAL);
	rc = find_installed(dst, dst_size, relative_path);
	if (rc != 0)
		MORPH_RETURN(rc);
	return 0;
}

char *morph_data_find_alloc(const char *relative_path)
{
	char path[PATH_MAX];

	if (morph_data_find(path, sizeof(path), relative_path) != 0)
		return NULL;
	return strdup(path);
}
