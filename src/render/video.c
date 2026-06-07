#include "video.h"
#include "util/error.h"
#include "util/log.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_MPV_ARGS 16

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
