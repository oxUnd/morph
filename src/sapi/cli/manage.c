#include "util/signals.h"
#include "sapi/cli/manage.h"
#include "util/error.h"
#include "util/file.h"
#include "util/name.h"
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

void cli_manage_reset_getopt(void)
{
	opterr = 1;
#if defined(__GLIBC__)
	optind = 0;
#else
	optind = 1;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
	optreset = 1;
#endif
#endif
}

int cli_manage_error(int rc, const char *context)
{
	fprintf(stderr, "morph: %s: %s\n", context, morph_strerror(rc));
	return 1;
}

int cli_manage_copy_arg(char *dst, size_t cap, const char *src)
{
	if (strlen(src) >= cap)
		MORPH_RETURN(-E2BIG);
	memcpy(dst, src, strlen(src) + 1);
	return 0;
}

int cli_manage_name_valid(const char *name)
{
	return morph_name_valid(name, "-_");
}

int cli_manage_run(const char *dir, char *const argv[], morph_buf_t *output)
{
	int descriptors[2];
	int status = 0;
	int rc = 0;
	pid_t pid;
	char buffer[BUFSIZ];

	if (pipe(descriptors) != 0)
		MORPH_RETURN_ERRNO();
	pid = fork();
	if (pid < 0) {
		int error = -errno;

		close(descriptors[0]);
		close(descriptors[1]);
		MORPH_RETURN(error);
	}
	if (pid == 0) {
		morph_signals_reset_child();
		int null_fd = open("/dev/null", O_RDONLY);

		close(descriptors[0]);
		if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0 ||
		    dup2(descriptors[1], STDOUT_FILENO) < 0)
			_exit(126);
		close(null_fd);
		close(descriptors[1]);
		if (dir && chdir(dir) != 0)
			_exit(126);
		execvp(argv[0], argv);
		_exit(errno == ENOENT ? 127 : 126);
	}
	close(descriptors[1]);
	for (;;) {
		ssize_t count = read(descriptors[0], buffer, sizeof(buffer));

		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			MORPH_SET_ERR(rc, -errno);
			break;
		}
		if (count == 0)
			break;
		if (output && rc == 0) {
			if (output->len + (size_t)count > 1024 * 1024)
				MORPH_SET_ERR(rc, -EFBIG);
			else
				rc = morph_buf_append(output, buffer, (size_t)count);
		}
	}
	close(descriptors[0]);
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			MORPH_RETURN_ERRNO();
	}
	if (rc != 0)
		return rc;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		MORPH_RETURN(MORPH_ERR_PROCESSING);
	return 0;
}

int cli_manage_atomic_write(const char *path, const char *data, size_t len)
{
	char *temporary = file_path_append_alloc(path, ".tmp-XXXXXX");
	struct stat st;
	size_t offset = 0;
	int rc = 0;
	int fd;

	if (!temporary)
		MORPH_RETURN(-ENOMEM);
	fd = mkstemp(temporary);
	if (fd < 0) {
		int error = -errno;

		free(temporary);
		MORPH_RETURN(error);
	}
	if (stat(path, &st) == 0 && fchmod(fd, st.st_mode & 0777) != 0)
		MORPH_SET_ERR(rc, -errno);
	while (rc == 0 && offset < len) {
		ssize_t count = write(fd, data + offset, len - offset);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			MORPH_SET_ERR(rc, count < 0 ? -errno : -EIO);
		else
			offset += (size_t)count;
	}
	if (rc == 0 && fsync(fd) != 0)
		MORPH_SET_ERR(rc, -errno);
	if (close(fd) != 0 && rc == 0)
		MORPH_SET_ERR(rc, -errno);
	if (rc == 0 && rename(temporary, path) != 0)
		MORPH_SET_ERR(rc, -errno);
	if (rc != 0)
		unlink(temporary);
	free(temporary);
	return rc;
}

int cli_manage_lock(const char *path)
{
	int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);

	if (fd < 0)
		MORPH_RETURN_ERRNO();
	if (flock(fd, LOCK_EX) != 0) {
		int error = -errno;

		close(fd);
		MORPH_RETURN(error);
	}
	return fd;
}
