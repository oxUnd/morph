#include "sapi/cli/services/file_preview.h"

#include "sapi/cli/adapters/media/image_probe.h"
#include "terminal/text.h"
#include <morph_markdown_highlight.h>
#include <nb/cancel.h>
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/thread.h>
#include <nb/utf8.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define CLI_FILE_PREVIEW_TEXT_LIMIT (1024u * 1024u)
#define CLI_FILE_PREVIEW_IMAGE_LIMIT (32u * 1024u * 1024u)

struct cli_file_preview_job {
	nb_thread *thread;
	struct nb_cancel cancel;
	nb_atomic_int done;
	struct cli_file_preview *preview;
};

void cli_file_preview_free(struct cli_file_preview *preview)
{
	if (!preview)
		return;

	free(preview->path);
	free(preview->highlighted);
	nb_buf_cleanup(&preview->content);
	free(preview);
}

void cli_file_preview_cleanup(struct cli_file_preview **preview)
{
	cli_file_preview_free(*preview);
	*preview = NULL;
}

static void cli_file_preview_fd_cleanup(int *fd)
{
	if (*fd >= 0)
		(void)close(*fd);
}

static void cli_file_preview_stream_cleanup(FILE **file)
{
	if (*file)
		(void)fclose(*file);
}

static int cli_file_preview_read(struct cli_file_preview_job *job)
{
	struct cli_file_preview *preview = job->preview;
	int fd NB_CLEANUP(cli_file_preview_fd_cleanup) =
		open(preview->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return nb_status_from_errno(errno, NULL);

	FILE *file NB_CLEANUP(cli_file_preview_stream_cleanup) = fdopen(fd, "rb");
	if (!file)
		return nb_status_from_errno(errno, NULL);

	fd = -1;
	struct stat info;
	if (fstat(fileno(file), &info))
		return nb_status_from_errno(errno, NULL);

	if (!S_ISREG(info.st_mode))
		return NB_ERR_UNSUPPORTED;

	if (info.st_size > (off_t)CLI_FILE_PREVIEW_IMAGE_LIMIT)
		return NB_ERR_TOO_BIG;

	if (nb_cancel_requested(&job->cancel))
		return NB_ERR_CANCELLED;

	int width = 0, height = 0;
	if (!cli_media_image_probe_file(file, &width, &height)) {
		preview->image = 1;
		return NB_OK;
	}

	if (fseek(file, 0, SEEK_SET))
		return nb_status_from_errno(errno, NULL);

	const char *extension = strrchr(preview->path, '.');
	preview->markdown = extension &&
		(!strcasecmp(extension, ".md") || !strcasecmp(extension, ".markdown"));
	char bytes[BUFSIZ];
	while (preview->content.len <= CLI_FILE_PREVIEW_TEXT_LIMIT) {
		if (nb_cancel_requested(&job->cancel))
			return NB_ERR_CANCELLED;

		size_t remaining = CLI_FILE_PREVIEW_TEXT_LIMIT + 1u - preview->content.len;
		size_t capacity = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
		size_t count = fread(bytes, 1, capacity, file);
		if (!count && ferror(file)) {
			int saved = errno;
			if (saved == EINTR) {
				clearerr(file);
				continue;
			}

			return nb_status_from_errno(saved, NULL);
		}

		if (!count)
			break;

		if (!nb_buf_append(&preview->content, bytes, count))
			return nb_buf_status(&preview->content);
	}

	if (preview->content.len > CLI_FILE_PREVIEW_TEXT_LIMIT) {
		preview->truncated = 1;
		size_t length = nb_utf8_clamp_bytes(nb_buf_cstr(&preview->content),
			CLI_FILE_PREVIEW_TEXT_LIMIT);
		preview->content.len = length;
		preview->content.ptr[length] = '\0';
	}

	if (memchr(preview->content.ptr, '\0', preview->content.len) ||
	    nb_utf8_validate(nb_buf_str(&preview->content), NULL) != NB_UTF8_OK) {
		nb_buf_clear(&preview->content);
		return NB_ERR_FORMAT;
	}

	return NB_OK;
}

static int cli_file_preview_highlight(struct cli_file_preview_job *job)
{
	struct cli_file_preview *preview = job->preview;
	if (preview->image || preview->markdown)
		return NB_OK;

	const char *language = morph_md_highlight_language(preview->path,
		nb_buf_cstr(&preview->content), preview->content.len);
	if (!language)
		return NB_OK;

	if (nb_cancel_requested(&job->cancel))
		return NB_ERR_CANCELLED;

	size_t length = 0;
	char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		nb_buf_cstr(&preview->content), preview->content.len,
		MORPH_TERMINAL_TEXT_MULTILINE, &length);
	if (!safe)
		return NB_ERR_NOMEM;

	int rc = morph_md_highlight_ansi(language, safe, length, &preview->highlighted, NULL);
	return rc ? nb_status_from_errno(-rc, NULL) : NB_OK;
}

static void *cli_file_preview_worker(void *data)
{
	struct cli_file_preview_job *job = data;
	job->preview->status = cli_file_preview_read(job);
	if (!job->preview->status)
		job->preview->status = cli_file_preview_highlight(job);
	nb_atomic_int_store(&job->done, 1);
	return NULL;
}

void cli_file_preview_stop(struct cli_file_preview_job **owner)
{
	struct cli_file_preview_job *job = *owner;
	if (!job)
		return;

	nb_cancel_request(&job->cancel);
	if (job->thread)
		(void)nb_thread_join_take(&job->thread, NULL);

	cli_file_preview_free(job->preview);
	free(job);
	*owner = NULL;
}

int cli_file_preview_start(const char *path, struct cli_file_preview_job **out)
{
	if (!path || !out)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	struct cli_file_preview *preview NB_CLEANUP(cli_file_preview_cleanup) =
		calloc(1, sizeof(*preview));
	if (!preview)
		return NB_ERR_NOMEM;

	preview->path = strdup(path);
	if (!preview->path)
		return NB_ERR_NOMEM;

	struct cli_file_preview_job *job NB_CLEANUP(cli_file_preview_stop) =
		calloc(1, sizeof(*job));
	if (!job)
		return NB_ERR_NOMEM;

	job->preview = preview;
	preview = NULL;
	nb_cancel_init(&job->cancel);
	nb_atomic_int_init(&job->done, 0);
	int rc = nb_thread_create(&job->thread, cli_file_preview_worker, job, NULL);
	if (rc)
		return rc;

	*out = job;
	job = NULL;
	return NB_OK;
}

int cli_file_preview_collect(struct cli_file_preview_job **owner, struct cli_file_preview **out)
{
	if (!owner || !*owner || !out)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	struct cli_file_preview_job *job = *owner;
	if (!nb_atomic_int_load(&job->done))
		return NB_ERR_AGAIN;

	(void)nb_thread_join_take(&job->thread, NULL);
	*out = job->preview;
	job->preview = NULL;
	cli_file_preview_stop(owner);
	return NB_OK;
}
