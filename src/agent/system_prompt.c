#include "system_prompt.h"
#include "errors.h"
#include "nb/err.h"
#include "nb/cleanup.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/file.h"
#include "nb/log.h"
#include "nb/utf8.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int prompt_append_file(nb_buf *buf, const char *path)
{
	struct stat st;
	FILE *stream;
	char chunk[BUFSIZ];
	size_t start = buf->len;
	size_t content_start;
	size_t n;
	int rc = 0;

	if (stat(path, &st) != 0)
		return nb_status_from_errno(errno, NULL);
	if (!S_ISREG(st.st_mode))
		return NB_ERR_INVAL;
	stream = fopen(path, "rb");
	if (!stream)
		return nb_status_from_errno(errno, NULL);
	if (start)
		rc = (nb_buf_append_cstr(buf, "\n\n") ? 0 : NB_ERR_NOMEM);
	content_start = buf->len;
	while (rc == 0 && (n = fread(chunk, 1, sizeof(chunk), stream)) > 0)
		rc = (nb_buf_append(buf, chunk, n) ? 0 : NB_ERR_NOMEM);
	if (rc == 0 && ferror(stream))
		rc = errno ? nb_status_from_errno(errno, NULL) : NB_ERR_IO;
	if (fclose(stream) != 0 && rc == 0)
		rc = nb_status_from_errno(errno, NULL);
	if (rc != 0)
		return rc;

	n = buf->len - content_start;
	if (memchr(buf->ptr + content_start, '\0', n) ||
	    nb_utf8_validate(nb_str_from_parts(buf->ptr + content_start, n), NULL))
		return NB_ERR_ENCODING;
	while (n > 0 && strchr("\r\n \t", buf->ptr[content_start + n - 1]))
		n--;
	return (nb_buf_truncate(buf, n ? content_start + n : start), 0);
}

static int prompt_append_directory(struct nb_log *logger, nb_buf *buf, const char *dir)
{
	int directory_status_1 = NB_OK;

	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;

	nb_dir_listing files NB_CLEANUP(nb_dir_listing_destroy) = {0};
	int count = 0;
	int rc = ((directory_status_1 = nb_dir_scan(
			   dir, NB_DIR_SCAN_FILES | NB_DIR_SCAN_SORT | NB_DIR_SCAN_FOLLOW_LINKS,
			   &files, NULL)) != NB_OK
			  ? directory_status_1
		  : files.names.len > INT_MAX ? NB_ERR_RANGE
					      : (count = (int)files.names.len, NB_OK));

	if (rc != 0)
		return rc;
	for (int i = 0; i < count; i++) {
		char path[PATH_MAX];

		rc = ((file_status_1 = nb_path_concat(&file_value_1, dir,
						      nb_dir_listing_name(&files, (size_t)(i)))) !=
				      NB_OK
			      ? file_status_1
		      : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), path,
							sizeof(path))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_1);
		if (rc == 0)
			rc = prompt_append_file(buf, path);
		if (rc != 0) {
			nb_log_write(logger, NB_LOG_ERROR,
				     "cannot load prompt fragment '%s/%s': %s", dir,
				     nb_dir_listing_name(&files, (size_t)(i)),
				     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
			break;
		}
	}
	nb_dir_listing_destroy(&files);
	return rc;
}

static int prompt_append_source(struct nb_log *logger, nb_buf *buf, const char *source,
				int is_directory)
{
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};

	char *path = (nb_path_expand(&file_value_2, source, NULL) == NB_OK
			      ? strdup(nb_buf_cstr(&file_value_2))
			      : NULL);
	int rc;

	if (!path)
		return NB_ERR_NOMEM;
	rc = is_directory ? prompt_append_directory(logger, buf, path)
			  : prompt_append_file(buf, path);
	if (rc != 0)
		nb_log_write(logger, NB_LOG_ERROR, "cannot load prompt source '%s': %s", path,
			     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	free(path);
	return rc;
}

int morph_prompt_load(struct nb_log *logger, const char *file, const char *dir, char **out)
{
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc;

	if (!out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_STATE;
	if ((!file || !*file) && (!dir || !*dir))
		return 0;
	rc = (nb_buf_init(&buf), nb_buf_reserve(&buf, 1024) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	if (file && *file)
		rc = prompt_append_source(logger, &buf, file, 0);
	if (rc == 0 && dir && *dir)
		rc = prompt_append_source(logger, &buf, dir, 1);
	if (rc == 0) {
		*out = (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
		if (!*out)
			rc = NB_ERR_NOMEM;
	}
	nb_buf_cleanup(&buf);
	return rc;
}
