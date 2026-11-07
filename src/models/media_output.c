#include "media_output.h"
#include "util/error.h"
#include "util/file.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int media_write_fd(int fd, const void *data, size_t len)
{
	const unsigned char *bytes = data;

	while (len > 0) {
		size_t chunk = len > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : len;
		ssize_t written = write(fd, bytes, chunk);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return morph_error_from_errno(errno);
		}
		if (written == 0)
			return -EIO;
		bytes += written;
		len -= (size_t)written;
	}
	return 0;
}

static int media_output_attempt(const char *output_dir, const char *prefix, const char *extension,
				const void *data, size_t len, char *path, size_t path_size)
{
	char temporary[PATH_MAX];
	char destination[PATH_MAX];
	char *directory;
	int fd;
	int length;
	int rc;

	if (!path || path_size == 0)
		return -EINVAL;
	path[0] = '\0';
	if (!prefix || !*prefix || !extension || !*extension || strchr(prefix, '/') ||
	    strchr(extension, '/') || (!data && len))
		return -EINVAL;
	directory =
		file_expand_path_copy(output_dir && *output_dir ? output_dir : "~/.morph/output");
	if (!directory)
		return -ENOMEM;
	rc = file_ensure_dir(directory);
	if (rc != 0) {
		free(directory);
		return rc;
	}
	length = snprintf(temporary, sizeof(temporary), "%s/.morph-media-XXXXXX", directory);
	if (length < 0 || (size_t)length >= sizeof(temporary)) {
		free(directory);
		return -ENAMETOOLONG;
	}
	fd = mkstemp(temporary);
	if (fd < 0) {
		rc = morph_error_from_errno(errno);
		free(directory);
		return rc;
	}
	length = snprintf(destination, sizeof(destination), "%s/%s_%s.%s", directory, prefix,
			  temporary + strlen(temporary) - 6, extension);
	free(directory);
	if (length < 0 || (size_t)length >= sizeof(destination) || (size_t)length >= path_size)
		rc = -ENAMETOOLONG;
	else
		rc = media_write_fd(fd, data, len);
	if (close(fd) != 0 && rc == 0)
		rc = morph_error_from_errno(errno);
	/* link publishes the completed file without replacing an existing path. */
	if (rc == 0 && link(temporary, destination) != 0)
		rc = morph_error_from_errno(errno);
	(void)unlink(temporary);
	if (rc == 0)
		memcpy(path, destination, (size_t)length + 1);
	return rc;
}

int media_output_write(const char *output_dir, const char *prefix, const char *extension,
		       const void *data, size_t len, char *path, size_t path_size)
{
	int rc;

	for (int attempt = 0; attempt < 16; attempt++) {
		rc = media_output_attempt(output_dir, prefix, extension, data, len, path,
					  path_size);
		if (rc != -EEXIST)
			return rc;
	}
	return rc;
}
