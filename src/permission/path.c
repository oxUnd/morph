#include "path.h"
#include "context.h"
#include "util/error.h"
#include "util/file.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int permission_path_resolve_absolute(const char *path, char *resolved, size_t size)
{
	char *canonical;
	int rc = 0;

	if (!path || !*path || !resolved || !size || !file_path_is_absolute(path))
		return -EINVAL;
	if (strlen(path) >= PATH_MAX)
		return -ENAMETOOLONG;
	canonical = file_resolve_path_copy(path);
	if (!canonical)
		return -ENOMEM;
	if (strlen(canonical) >= size)
		rc = -ENAMETOOLONG;
	else
		memcpy(resolved, canonical, strlen(canonical) + 1);
	free(canonical);
	return rc;
}

int permission_path_resolve_existing_directory(const char *path, int create, char *resolved,
					       size_t size)
{
	char *expanded;
	struct stat st;
	int rc;

	if (!path || !*path || !resolved || !size)
		return -EINVAL;
	expanded = file_expand_path_copy(path);
	if (!expanded)
		return -ENOMEM;
	if (!file_path_is_absolute(expanded)) {
		free(expanded);
		return -EINVAL;
	}
	rc = create ? file_ensure_dir(expanded) : 0;
	if (rc == 0 && stat(expanded, &st) != 0)
		rc = morph_error_from_errno(errno);
	else if (rc == 0 && !S_ISDIR(st.st_mode))
		rc = -ENOTDIR;
	if (rc == 0)
		rc = permission_path_resolve_absolute(expanded, resolved, size);
	free(expanded);
	return rc;
}

const char *tool_path_access_name(unsigned int access)
{
	static const char *const names[] = {
		"",	  "read",	 "write",	 "read/write",
		"delete", "read/delete", "write/delete", "read/write/delete",
	};

	return access <= TOOL_PATH_ACCESS_ALL ? names[access] : "";
}

int tool_context_resolve_directory_access(const char *path, unsigned int access, char *resolved,
					  size_t resolved_size)
{
	if (!path || !*path || !access || (access & ~(unsigned int)TOOL_PATH_ACCESS_ALL) ||
	    !resolved || !resolved_size)
		return -EINVAL;
	char *expanded = file_expand_path_copy(path);

	if (!expanded)
		return -ENOMEM;
	if (!file_path_is_absolute(expanded) || strlen(expanded) >= PATH_MAX) {
		int rc = strlen(expanded) >= PATH_MAX ? -ENAMETOOLONG : -EINVAL;

		free(expanded);
		return rc;
	}
	char *canonical = file_resolve_path_copy(expanded);

	free(expanded);
	if (!canonical)
		return -ENOMEM;
	struct stat st;
	int rc = 0;

	if (stat(canonical, &st) != 0) {
		if (errno != ENOENT || !(access & TOOL_PATH_ACCESS_WRITE))
			rc = (-errno);
	} else if (!S_ISDIR(st.st_mode)) {
		rc = (-ENOTDIR);
	}
	if (rc == 0 && strlen(canonical) >= resolved_size)
		rc = (-ENAMETOOLONG);
	if (rc == 0)
		memcpy(resolved, canonical, strlen(canonical) + 1);
	free(canonical);
	return rc;
}
