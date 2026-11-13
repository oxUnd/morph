#include "media_output.h"
#include "nb/cleanup.h"
#include "nb/file.h"
#include "nb/random.h"
#include <string.h>

static int media_output_attempt(const char *output_dir, const char *prefix, const char *extension,
				const void *data, size_t len, char *path, size_t path_size)
{
	nb_buf directory NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf name NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf destination NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_file_write_options options = {.mode = 0600, .create_only = true, .sync_dir = true};

	if (!path || path_size == 0)
		return NB_ERR_INVAL;
	path[0] = '\0';
	if (!prefix || !*prefix || !extension || !*extension || strchr(prefix, '/') ||
	    strchr(extension, '/') || (!data && len))
		return NB_ERR_INVAL;
	int rc = nb_path_expand(&directory,
				output_dir && *output_dir ? output_dir : "~/.morph/output", NULL);
	if (rc != 0)
		return rc;
	rc = nb_file_mkdir(nb_buf_cstr(&directory), true, NULL);
	if (rc != 0)
		return rc;
	if (!nb_buf_printf(&name, "%s_", prefix))
		return nb_buf_status(&name);
	rc = nb_random_hex(&name, 8, NULL);
	if (rc != 0)
		return rc;
	if (!nb_buf_printf(&name, ".%s", extension))
		return nb_buf_status(&name);
	rc = nb_path_concat(&destination, nb_buf_cstr(&directory), nb_buf_cstr(&name));
	if (rc != 0)
		return rc;
	if (nb_buf_len(&destination) >= path_size)
		return NB_ERR_NAME_TOO_LONG;
	rc = nb_file_write_all_atomic(nb_buf_cstr(&destination), data, len, &options, NULL);
	if (rc == 0)
		memcpy(path, nb_buf_cstr(&destination), nb_buf_len(&destination) + 1);
	return rc;
}

int media_output_write(const char *output_dir, const char *prefix, const char *extension,
		       const void *data, size_t len, char *path, size_t path_size)
{
	int rc;

	for (int attempt = 0; attempt < 16; attempt++) {
		rc = media_output_attempt(output_dir, prefix, extension, data, len, path,
					  path_size);
		if (rc != NB_ERR_EXISTS)
			return rc;
	}
	return rc;
}
