#include "nb/process.h"
#include "video.h"
#include "image.h"
#include "nb/buf.h"
#include "nb/file.h"
#include "nb/str.h"
#include "nb/encoding.h"
#include "nb/cleanup.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/log.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define MAX_MPV_ARGS 16

int video_file_url_append(const char *path, nb_buf *url)
{
	if (!path || !url)
		return NB_ERR_INVAL;
	if (!nb_buf_append_cstr(url, "file://"))
		return NB_ERR_NOMEM;
	nb_split_iter segments;
	nb_str segment;
	nb_split_init(&segments, nb_str_from_cstr(path), '/');
	int first = 1;
	while (nb_split_next(&segments, &segment)) {
		if (!first && !nb_buf_append_char(url, '/'))
			return NB_ERR_NOMEM;
		first = 0;
		int rc = nb_url_encode(segment, false, url, NULL);
		if (rc != NB_OK)
			return rc;
	}
	return NB_OK;
}

static int video_extract_preview(const char *path, char *preview,
				 size_t preview_size)
{
	struct stat info;
	nb_buf temporary NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!path || !preview || !preview_size)
		return NB_ERR_INVAL;
	int rc = nb_file_temp_in(&temporary, "/tmp", NULL, NULL);
	if (rc != NB_OK)
		return rc;
	rc = nb_str_copy_to(nb_buf_str(&temporary), preview, preview_size);
	if (rc != NB_OK) {
		(void)nb_file_remove(nb_buf_cstr(&temporary), NULL);
		return rc == NB_ERR_RANGE ? NB_ERR_NAME_TOO_LONG : rc;
	}

	const char *argv[] = {"ffmpeg",	 "-loglevel", "error",	   "-y", "-ss", "0",
			      "-i",	 path,	      "-frames:v", "1",	 "-f",	"image2",
			      "-vcodec", "png",	      preview,	   NULL};
	nb_process_options options = {
		.argv = argv,
		.search_path = true,
		.close_fds = true,
		.stdin_mode = NB_PROCESS_NULL,
		.stdout_mode = NB_PROCESS_NULL,
		.stderr_mode = NB_PROCESS_NULL,
	};
	nb_process *child NB_CLEANUP(nb_process_cleanup) = NULL;
	rc = nb_process_spawn(&options, &child, NULL);
	int code = 0;
	if (rc == NB_OK)
		rc = nb_process_wait(child, -1, &code, NULL, NULL);
	if (rc == NB_OK && code != 0)
		rc = code == 127 ? NB_ERR_NOT_FOUND : NB_ERR_IO;
	if (rc != 0 || stat(preview, &info) != 0 ||
	    !S_ISREG(info.st_mode) || info.st_size <= 0) {
		unlink(preview);
		return rc != 0 ? rc : NB_ERR_IO;
	}
	return 0;
}

int video_preview_create_copy(const char *path, char **out_path)
{
	if (!out_path || *out_path)
		return NB_ERR_INVAL;
	char preview[PATH_MAX];
	int rc = video_extract_preview(path, preview, sizeof(preview));
	if (rc)
		return rc;
	char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(preview);
	if (!copy) {
		unlink(preview);
		return NB_ERR_NOMEM;
	}
	*out_path = nb_charp_steal(&copy);
	return 0;
}

static void video_print_open_link(const char *url)
{
	printf("\033]8;;%s\033\\▶ Open video\033]8;;\033\\\n", url);
	fflush(stdout);
}

int video_render_terminal_preview(struct nb_log *logger, const char *path)
{
	nb_buf url NB_CLEANUP(nb_buf_cleanup) = {0};
	char preview[PATH_MAX];
	int rc;

	if (!path || !path[0])
		return NB_ERR_INVAL;
	rc = (nb_buf_init(&url), nb_buf_reserve(&url, 128u) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = video_file_url_append(path, &url);
	if (rc != 0) {
		nb_buf_cleanup(&url);
		return rc;
	}
	if (image_terminal_protocol_available() &&
	    video_extract_preview(path, preview, sizeof(preview)) == 0) {
		printf("\n\033]8;;%s\033\\", nb_buf_cstr(&url));
		fflush(stdout);
		rc = image_render_terminal(logger, preview);
		printf("\033]8;;\033\\");
		unlink(preview);
		if (rc == 0) {
			video_print_open_link(nb_buf_cstr(&url));
			nb_buf_cleanup(&url);
			return 0;
		}
	}
	video_print_open_link(nb_buf_cstr(&url));
	nb_buf_cleanup(&url);
	return 0;
}

int video_play(struct nb_log *logger, const char *path, const char *mpv_args)
{
	if (!path) {
		nb_log_write(logger, NB_LOG_ERROR, "video_play: null path");
		return NB_ERR_INVAL;
	}

	char *argv[MAX_MPV_ARGS];
	char *buf = NULL;

	int n = 0;
	argv[n++] = "mpv";

	if (mpv_args && mpv_args[0]) {
		buf = strdup(mpv_args);
		if (!buf)
			return NB_ERR_NOMEM;
		char *save = NULL;
		char *tok = strtok_r(buf, " ", &save);
		while (tok && n < MAX_MPV_ARGS - 2) {
			argv[n++] = tok;
			tok = strtok_r(NULL, " ", &save);
		}
	}
	argv[n++] = (char *)path;
	argv[n] = NULL;

	nb_process_options options = {
		.argv = (const char *const *)argv,
		.search_path = true,
		.close_fds = true,
		.stdin_mode = NB_PROCESS_INHERIT,
		.stdout_mode = NB_PROCESS_INHERIT,
		.stderr_mode = NB_PROCESS_INHERIT,
	};
	nb_process *child NB_CLEANUP(nb_process_cleanup) = NULL;
	int rc = nb_process_spawn(&options, &child, NULL);
	free(buf);
	if (rc != NB_OK)
		return rc;
	int code = 0;
	rc = nb_process_wait(child, -1, &code, NULL, NULL);
	if (rc == NB_OK && code != 0)
		rc = code == 127 ? NB_ERR_NOT_FOUND : NB_ERR_IO;
	return rc;
}
