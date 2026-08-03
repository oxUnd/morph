#include "video.h"
#include "image.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/log.h"
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

static int video_file_url(const char *path, morph_buf_t *url)
{
	const unsigned char *cursor;
	int rc;

	if (!path || !url)
		MORPH_RETURN(-EINVAL);
	rc = morph_buf_puts(url, "file://");
	for (cursor = (const unsigned char *)path; rc == 0 && *cursor;
	     cursor++) {
		if (isalnum(*cursor) || strchr("-._~/", *cursor))
			rc = morph_buf_putc(url, (char)*cursor);
		else
			rc = morph_buf_printf(url, "%%%02X", *cursor);
	}
	return rc;
}

static int video_wait_child(pid_t pid)
{
	int status;
	pid_t waited;

	do {
		waited = waitpid(pid, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited < 0)
		MORPH_RETURN(-errno);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
			MORPH_RETURN(-ENOENT);
		MORPH_RETURN(-EIO);
	}
	return 0;
}

static int video_extract_preview(const char *path, char *preview,
				 size_t preview_size)
{
	struct stat info;
	pid_t pid;
	int fd;
	int rc;

	if (!path || !preview || preview_size < sizeof("/tmp/morph-video-XXXXXX"))
		MORPH_RETURN(-EINVAL);
	strncpy(preview, "/tmp/morph-video-XXXXXX", preview_size - 1u);
	preview[preview_size - 1u] = '\0';
	fd = mkstemp(preview);
	if (fd < 0)
		MORPH_RETURN(-errno);
	close(fd);
	pid = fork();
	if (pid < 0) {
		rc = -errno;
		unlink(preview);
		MORPH_RETURN(rc);
	}
	if (pid == 0) {
		int null_fd = open("/dev/null", O_WRONLY);
		char *const argv[] = {
			(char *)"ffmpeg", (char *)"-loglevel", (char *)"error",
			(char *)"-y", (char *)"-ss", (char *)"0",
			(char *)"-i", (char *)path, (char *)"-frames:v",
			(char *)"1", (char *)"-f", (char *)"image2",
			(char *)"-vcodec", (char *)"png", preview, NULL
		};

		if (null_fd >= 0) {
			(void)dup2(null_fd, STDOUT_FILENO);
			(void)dup2(null_fd, STDERR_FILENO);
			close(null_fd);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	rc = video_wait_child(pid);
	if (rc != 0 || stat(preview, &info) != 0 ||
	    !S_ISREG(info.st_mode) || info.st_size <= 0) {
		unlink(preview);
		MORPH_RETURN(rc != 0 ? rc : -EIO);
	}
	return 0;
}

static void video_print_open_link(const char *url)
{
	printf("\033]8;;%s\033\\▶ Open video\033]8;;\033\\\n", url);
	fflush(stdout);
}

int video_render_terminal_preview(const char *path)
{
	morph_buf_t url;
	char preview[PATH_MAX];
	int rc;

	if (!path || !path[0])
		MORPH_RETURN(-EINVAL);
	rc = morph_buf_init(&url, 128u);
	if (rc != 0)
		return rc;
	rc = video_file_url(path, &url);
	if (rc != 0) {
		morph_buf_cleanup(&url);
		return rc;
	}
	if (image_terminal_protocol_available() &&
	    video_extract_preview(path, preview, sizeof(preview)) == 0) {
		printf("\n\033]8;;%s\033\\", morph_buf_cstr(&url));
		fflush(stdout);
		rc = image_render_terminal(preview);
		printf("\033]8;;\033\\");
		unlink(preview);
		if (rc == 0) {
			video_print_open_link(morph_buf_cstr(&url));
			morph_buf_cleanup(&url);
			return 0;
		}
	}
	video_print_open_link(morph_buf_cstr(&url));
	morph_buf_cleanup(&url);
	return 0;
}

int video_play(const char *path, const char *mpv_args)
{
	if (!path) {
		log_err("video_play: null path");
		return -EINVAL;
	}

	char *argv[MAX_MPV_ARGS];
	char *buf = NULL;

	int n = 0;
	argv[n++] = "mpv";

	if (mpv_args && mpv_args[0]) {
		buf = strdup(mpv_args);
		if (!buf) return -ENOMEM;
		char *save = NULL;
		char *tok = strtok_r(buf, " ", &save);
		while (tok && n < MAX_MPV_ARGS - 2) {
			argv[n++] = tok;
			tok = strtok_r(NULL, " ", &save);
		}
	}
	argv[n++] = (char *)path;
	argv[n] = NULL;

	pid_t pid = fork();
	if (pid < 0) {
		int err = errno;
		log_err("video_play: fork failed");
		free(buf);
		MORPH_RETURN(-err);
	}

	if (pid == 0) {
		execvp("mpv", argv);
		_exit(127);
	}

	free(buf);

	int wstatus;
	waitpid(pid, &wstatus, 0);
	if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 127) {
		log_err("video_play: mpv not found");
		return -ENOENT;
	}
	return 0;
}
