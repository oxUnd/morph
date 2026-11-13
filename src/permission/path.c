#include "path.h"
#include "nb/cleanup.h"
#include "context.h"
#include "errors.h"
#include "nb/file.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int permission_path_resolve_absolute(const char *path, char *resolved, size_t size)
{
	nb_buf canonical NB_CLEANUP(nb_buf_destroy) = {0};
	if (!path || !*path || !resolved || !size || !nb_path_is_absolute(path))
		return NB_ERR_INVAL;
	if (strlen(path) >= PATH_MAX)
		return NB_ERR_NAME_TOO_LONG;
	int rc = nb_path_canonical_missing(&canonical, path, NULL);
	if (rc != NB_OK)
		return rc;
	rc = nb_str_copy_to(nb_buf_str(&canonical), resolved, size);
	return rc == NB_ERR_RANGE ? NB_ERR_NAME_TOO_LONG : rc;
}

int permission_path_resolve_existing_directory(const char *path, int create, char *resolved,
					       size_t size)
{
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};

	char *expanded;
	struct stat st;
	int rc;

	if (!path || !*path || !resolved || !size)
		return NB_ERR_INVAL;
	expanded = (nb_path_expand(&file_value_2, path, NULL) == NB_OK
			    ? strdup(nb_buf_cstr(&file_value_2))
			    : NULL);
	if (!expanded)
		return NB_ERR_NOMEM;
	if (!nb_path_is_absolute(expanded)) {
		free(expanded);
		return NB_ERR_INVAL;
	}
	rc = create ? nb_file_mkdir(expanded, true, NULL) : 0;
	if (rc == 0 && stat(expanded, &st) != 0)
		rc = nb_status_from_errno(errno, NULL);
	else if (rc == 0 && !S_ISDIR(st.st_mode))
		rc = NB_ERR_NOT_DIR;
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
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {0};

	if (!path || !*path || !access || (access & ~(unsigned int)TOOL_PATH_ACCESS_ALL) ||
	    !resolved || !resolved_size)
		return NB_ERR_INVAL;
	char *expanded = (nb_path_expand(&file_value_3, path, NULL) == NB_OK
				  ? strdup(nb_buf_cstr(&file_value_3))
				  : NULL);

	if (!expanded)
		return NB_ERR_NOMEM;
	if (!nb_path_is_absolute(expanded) || strlen(expanded) >= PATH_MAX) {
		int rc = strlen(expanded) >= PATH_MAX ? NB_ERR_NAME_TOO_LONG : NB_ERR_INVAL;

		free(expanded);
		return rc;
	}
	char *canonical =
		(nb_path_expand(&file_value_4, expanded, NULL) == NB_OK &&
				 nb_path_canonical_missing(
					 &file_value_4, nb_buf_cstr(&file_value_4), NULL) == NB_OK
			 ? strdup(nb_buf_cstr(&file_value_4))
			 : NULL);

	free(expanded);
	if (!canonical)
		return NB_ERR_NOMEM;
	struct stat st;
	int rc = 0;

	if (stat(canonical, &st) != 0) {
		if (errno != ENOENT || !(access & TOOL_PATH_ACCESS_WRITE))
			rc = (nb_status_from_errno(errno, NULL));
	} else if (!S_ISDIR(st.st_mode)) {
		rc = (NB_ERR_NOT_DIR);
	}
	if (rc == 0 && strlen(canonical) >= resolved_size)
		rc = (NB_ERR_NAME_TOO_LONG);
	if (rc == 0)
		memcpy(resolved, canonical, strlen(canonical) + 1);
	free(canonical);
	return rc;
}
