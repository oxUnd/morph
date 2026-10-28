#include "sapi/cli/share.h"
#include "util/file.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int share_write(int fd, const morph_buf_t *html)
{
	for (size_t written = 0; written < html->len;) {
		ssize_t size = write(fd, html->data + written, html->len - written);
		if (size < 0 && errno == EINTR)
			continue;
		if (size <= 0)
			MORPH_RETURN(size < 0 ? -errno : -EIO);
		written += (size_t)size;
	}
	if (fsync(fd) != 0)
		MORPH_RETURN_ERRNO();
	return 0;
}

/* The staging file is private; readers see the final name only after a full write. */
static int share_publish(const char *directory, const char *stamp,
			 const morph_buf_t *html, char **out_path)
{
	morph_buf_t staging = {0};
	int rc = morph_buf_init(&staging, 128);
	if (rc)
		MORPH_RETURN(rc);
	rc = morph_buf_printf(&staging, "%s/.morph-share-%s-XXXXXX", directory, stamp);
	if (rc) {
		morph_buf_cleanup(&staging);
		MORPH_RETURN(rc);
	}
	int fd = mkstemp(staging.data);
	if (fd < 0) {
		MORPH_SET_ERRNO(rc);
		morph_buf_cleanup(&staging);
		MORPH_RETURN(rc);
	}
	/* Remove the staging basename's leading dot for the public filename. */
	const char *base = strrchr(staging.data, '/') + 1;
	char *target = file_path_join_alloc(directory, base + 1);
	char *published = target ? file_path_append_alloc(target, ".html") : NULL;
	free(target);
	int linked = 0;
	if (!published)
		MORPH_SET_ERR(rc, -ENOMEM);
	else
		rc = share_write(fd, html);
	if (close(fd) != 0 && !rc)
		MORPH_SET_ERRNO(rc);
	if (!rc) {
		if (link(staging.data, published) != 0)
			MORPH_SET_ERRNO(rc);
		else
			linked = 1;
	}
	if (unlink(staging.data) != 0 && !rc)
		MORPH_SET_ERRNO(rc);
	if (rc) {
		if (linked)
			(void)unlink(published);
		free(published);
	} else {
		*out_path = published;
	}
	morph_buf_cleanup(&staging);
	MORPH_RETURN(rc);
}

int cli_share_save(const cJSON *document, char **out_path)
{
	if (!out_path)
		MORPH_RETURN(-EINVAL);
	*out_path = NULL;
	morph_buf_t html = {0};
	int rc = morph_buf_init(&html, 4096);
	if (rc)
		MORPH_RETURN(rc);
	char *directory = NULL;
	time_t now = time(NULL);
	rc = cli_share_html(document, now, &html);
	if (rc)
		goto out_free_document;
	const char *home = getenv("HOME");
	if (!home || !home[0]) {
		MORPH_SET_ERR(rc, -ENOENT);
		goto out_free_document;
	}
	directory = file_path_join_alloc(home, "Downloads");
	if (!directory) {
		MORPH_SET_ERR(rc, -ENOMEM);
		goto out_free_document;
	}
	rc = file_ensure_dir(directory);
	if (rc)
		goto out_free_document;
	struct tm local;
	char stamp[32];
	if (!localtime_r(&now, &local) ||
	    !strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local)) {
		MORPH_SET_ERR(rc, -EINVAL);
		goto out_free_document;
	}
	rc = share_publish(directory, stamp, &html, out_path);
out_free_document:
	free(directory);
	morph_buf_cleanup(&html);
	MORPH_RETURN(rc);
}
