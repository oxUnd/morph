#include "sapi/cli/services/share_store.h"
#include "util/cleanup.h"
#include "cJSON.h"
#include "sapi/cli/presentation/share.h"
#include "util/file.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int cli_share_store_share_write(int fd, const morph_buf_t *html)
{
	for (size_t written = 0; written < html->len;) {
		ssize_t size = write(fd, html->data + written, html->len - written);
		if (size < 0 && errno == EINTR)
			continue;
		if (size <= 0)
			return size < 0 ? -errno : -EIO;
		written += (size_t)size;
	}
	if (fsync(fd) != 0)
		return morph_error_from_errno(errno);
	return 0;
}

/* The staging file is private; readers see the final name only after a full write. */
static int cli_share_store_share_publish(const char *directory, const char *stamp,
					 const morph_buf_t *html, char **out_path)
{
	morph_buf_t staging = {0};
	int rc = morph_buf_init(&staging, 128);
	if (rc)
		return rc;
	rc = morph_buf_printf(&staging, "%s/.morph-share-%s-XXXXXX", directory, stamp);
	if (rc) {
		morph_buf_cleanup(&staging);
		return rc;
	}
	int fd = mkstemp(staging.data);
	if (fd < 0) {
		rc = morph_error_from_errno(errno);
		morph_buf_cleanup(&staging);
		return rc;
	}
	/* Remove the staging basename's leading dot for the public filename. */
	const char *base = strrchr(staging.data, '/') + 1;
	char *target = file_path_join_alloc(directory, base + 1);
	char *published = target ? file_path_append_alloc(target, ".html") : NULL;
	free(target);
	int linked = 0;
	if (!published)
		rc = (-ENOMEM);
	else
		rc = cli_share_store_share_write(fd, html);
	if (close(fd) != 0 && !rc)
		rc = morph_error_from_errno(errno);
	if (!rc) {
		if (link(staging.data, published) != 0)
			rc = morph_error_from_errno(errno);
		else
			linked = 1;
	}
	if (unlink(staging.data) != 0 && !rc)
		rc = morph_error_from_errno(errno);
	if (rc) {
		if (linked)
			(void)unlink(published);
		free(published);
	} else {
		*out_path = published;
	}
	morph_buf_cleanup(&staging);
	return rc;
}

int cli_share_store_save(const cJSON *document, char **out_path)
{
	if (!out_path)
		return -EINVAL;
	if (*out_path)
		return -EEXIST;
	morph_buf_t html MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_buf_init(&html, 4096);
	if (rc)
		return rc;
	char *directory MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	time_t now = time(NULL);
	rc = cli_share_html(document, now, &html);
	if (rc)
		return rc;
	const char *home = getenv("HOME");
	if (!home || !home[0]) {
		rc = (-ENOENT);
		return rc;
	}
	directory = file_path_join_alloc(home, "Downloads");
	if (!directory) {
		rc = (-ENOMEM);
		return rc;
	}
	rc = file_ensure_dir(directory);
	if (rc)
		return rc;
	struct tm local;
	char stamp[32];
	if (!localtime_r(&now, &local) ||
	    !strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local)) {
		rc = (-EINVAL);
		return rc;
	}
	rc = cli_share_store_share_publish(directory, stamp, &html, out_path);
	return rc;
}
