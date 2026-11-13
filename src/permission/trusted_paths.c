#include "trusted_paths.h"
#include "nb/cleanup.h"
#include "nb/file.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void permission_temporary_paths_cleanup(struct permission_temporary_paths *paths)
{
	if (!paths)
		return;
	for (size_t i = 0; i < PERMISSION_TEMPORARY_ROOTS_MAX; i++)
		nb_buf_destroy(&paths->roots[i]);
	paths->count = 0;
}

static nb_status temporary_root_add(struct permission_temporary_paths *paths, const char *path,
				    const char *home, int custom)
{
	nb_buf root NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf canonical_home NB_CLEANUP(nb_buf_destroy) = {0};
	nb_file_info info;

	if (!path || !nb_path_is_absolute(path))
		return NB_OK;
	int rc = nb_path_canonical(&root, path, NULL);
	if (rc != NB_OK)
		return rc == NB_ERR_NOMEM ? NB_ERR_NOMEM : NB_OK;
	rc = nb_file_stat(nb_buf_cstr(&root), true, &info, NULL);
	if (rc != NB_OK || !info.is_dir)
		return NB_OK;
	if (custom) {
		if (nb_str_eq_cstr(nb_buf_str(&root), "/"))
			return NB_OK;
		if (home) {
			rc = nb_path_canonical(&canonical_home, home, NULL);
			if (rc == NB_ERR_NOMEM)
				return NB_ERR_NOMEM;
			if (rc == NB_OK && nb_path_is_within(nb_buf_cstr(&canonical_home),
							     nb_buf_cstr(&root), false))
				return NB_OK;
		}
	}
	for (size_t i = 0; i < paths->count; i++) {
		if (nb_str_eq(nb_buf_str(&paths->roots[i]), nb_buf_str(&root)))
			return NB_OK;
	}
	if (paths->count >= PERMISSION_TEMPORARY_ROOTS_MAX)
		return NB_ERR_RANGE;
	nb_buf_move(&paths->roots[paths->count], &root);
	paths->count++;
	return NB_OK;
}

nb_status permission_temporary_paths_collect(struct permission_temporary_paths *out,
					     const char *temporary, const char *home)
{
	struct permission_temporary_paths paths NB_CLEANUP(permission_temporary_paths_cleanup) = {
		0};
	if (!out)
		return NB_ERR_INVAL;
	for (size_t i = 0; i < PERMISSION_TEMPORARY_ROOTS_MAX; i++) {
		if (out->roots[i].ptr)
			return NB_ERR_STATE;
	}
	if (out->count)
		return NB_ERR_STATE;
	int rc = temporary_root_add(&paths, "/tmp", NULL, 0);
	if (rc == NB_OK)
		rc = temporary_root_add(&paths, "/var/tmp", NULL, 0);
#ifdef __APPLE__
	nb_buf user_temp NB_CLEANUP(nb_buf_destroy) = {0};
	size_t length = confstr(_CS_DARWIN_USER_TEMP_DIR, NULL, 0);
	if (rc == NB_OK && length) {
		if (!nb_buf_reserve(&user_temp, length))
			return NB_ERR_NOMEM;
		size_t actual = confstr(_CS_DARWIN_USER_TEMP_DIR, user_temp.ptr, user_temp.cap);
		if (actual > user_temp.cap)
			return NB_ERR_NAME_TOO_LONG;
		if (actual) {
			user_temp.len = actual - 1;
			rc = temporary_root_add(&paths, nb_buf_cstr(&user_temp), NULL, 0);
		}
	}
#endif
	if (rc == NB_OK)
		rc = temporary_root_add(&paths, temporary, home, 1);
	if (rc != NB_OK)
		return rc;
	*out = paths;
	memset(&paths, 0, sizeof(paths));
	return NB_OK;
}

int permission_temporary_path_is_default(const char *path)
{
	struct permission_temporary_paths paths NB_CLEANUP(permission_temporary_paths_cleanup) = {
		0};
	if (!path ||
	    permission_temporary_paths_collect(&paths, getenv("TMPDIR"), getenv("HOME")) != NB_OK)
		return 0;
	for (size_t i = 0; i < paths.count; i++) {
		if (nb_path_is_within(path, nb_buf_cstr(&paths.roots[i]), true))
			return 1;
	}
	return 0;
}
