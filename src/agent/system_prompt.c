#include "system_prompt.h"
#include "util/buf.h"
#include "util/file.h"
#include "util/log.h"
#include "util/utf8.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int prompt_append_file(morph_buf_t *buf, const char *path)
{
	struct stat st;
	FILE *stream;
	char chunk[BUFSIZ];
	size_t start = buf->len;
	size_t content_start;
	size_t n;
	int rc = 0;

	if (stat(path, &st) != 0)
		MORPH_RETURN_ERRNO();
	if (!S_ISREG(st.st_mode))
		MORPH_RETURN(-EINVAL);
	stream = fopen(path, "rb");
	if (!stream)
		MORPH_RETURN_ERRNO();
	if (start)
		rc = morph_buf_puts(buf, "\n\n");
	content_start = buf->len;
	while (rc == 0 && (n = fread(chunk, 1, sizeof(chunk), stream)) > 0)
		rc = morph_buf_append(buf, chunk, n);
	if (rc == 0 && ferror(stream))
		rc = errno ? -errno : -EIO;
	if (fclose(stream) != 0 && rc == 0)
		rc = -errno;
	if (rc != 0)
		MORPH_RETURN(rc);

	n = buf->len - content_start;
	if (memchr(buf->data + content_start, '\0', n) ||
	    utf8nvalid(buf->data + content_start, n))
		MORPH_RETURN(-EILSEQ);
	while (n > 0 && strchr("\r\n \t", buf->data[content_start + n - 1]))
		n--;
	return morph_buf_truncate(buf, n ? content_start + n : start);
}

static int prompt_append_directory(morph_buf_t *buf, const char *dir)
{
	char **files = NULL;
	int count = 0;
	int rc = file_list_files(dir, &files, &count);

	if (rc != 0)
		MORPH_RETURN(rc);
	for (int i = 0; i < count; i++) {
		char path[PATH_MAX];

		rc = file_path_join(path, sizeof(path), dir, files[i]);
		if (rc == 0)
			rc = prompt_append_file(buf, path);
		if (rc != 0) {
			log_err("cannot load prompt fragment '%s/%s': %s",
				dir, files[i], morph_strerror(rc));
			break;
		}
	}
	file_free_list(files, count);
	MORPH_RETURN(rc);
}

static int prompt_append_source(morph_buf_t *buf, const char *source,
			       int is_directory)
{
	char *path = file_expand_path(source);
	int rc;

	if (!path)
		MORPH_RETURN(-ENOMEM);
	rc = is_directory ? prompt_append_directory(buf, path) :
		prompt_append_file(buf, path);
	if (rc != 0)
		log_err("cannot load prompt source '%s': %s",
			path, morph_strerror(rc));
	free(path);
	MORPH_RETURN(rc);
}

int morph_prompt_load(const char *file, const char *dir, char **out)
{
	morph_buf_t buf;
	int rc;

	if (!out)
		MORPH_RETURN(-EINVAL);
	*out = NULL;
	if ((!file || !*file) && (!dir || !*dir))
		return 0;
	rc = morph_buf_init(&buf, 1024);
	if (rc != 0)
		MORPH_RETURN(rc);
	if (file && *file)
		rc = prompt_append_source(&buf, file, 0);
	if (rc == 0 && dir && *dir)
		rc = prompt_append_source(&buf, dir, 1);
	if (rc == 0)
		*out = morph_buf_detach(&buf);
	morph_buf_cleanup(&buf);
	MORPH_RETURN(rc);
}
