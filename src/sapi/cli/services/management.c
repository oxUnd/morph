#include "sapi/cli/services/management.h"
#include "nb/cleanup.h"
#include "nb/process.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/str.h"
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

void cli_management_reset_getopt(void)
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

int cli_management_error(int rc, const char *context)
{
	fprintf(stderr, "morph: %s: %s\n", context,
		nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	return 1;
}

int cli_management_copy_arg(char *dst, size_t cap, const char *src)
{
	if (strlen(src) >= cap)
		return NB_ERR_TOO_BIG;
	memcpy(dst, src, strlen(src) + 1);
	return 0;
}

int cli_management_name_valid(const char *name)
{
	return nb_str_is_alnum(nb_str_from_cstr(name), "-_");
}

int cli_management_run(const char *dir, char *const argv[], nb_buf *output)
{
	nb_process_options options = {
		.argv = (const char *const *)argv,
		.cwd = dir,
		.search_path = true,
		.close_fds = true,
		.new_group = true,
		.stdin_mode = NB_PROCESS_NULL,
		.stderr_mode = NB_PROCESS_INHERIT,
	};
	nb_process *child NB_CLEANUP(nb_process_cleanup) = NULL;
	int rc = nb_process_spawn(&options, &child, NULL);
	if (rc != NB_OK)
		return rc;
	nb_buf errors NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf discarded NB_CLEANUP(nb_buf_destroy) = {0};
	int code = 0;
	rc = nb_process_exchange(child, "", 0, NULL, output ? output : &discarded, &errors, &code,
				 NULL);
	if (rc != NB_OK)
		return rc;
	return code == 0 ? NB_OK : MORPH_ERR_PROCESSING;
}

int cli_management_lock(const char *path)
{
	int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);

	if (fd < 0)
		return nb_status_from_errno(errno, NULL);
	if (flock(fd, LOCK_EX) != 0) {
		int error = nb_status_from_errno(errno, NULL);

		close(fd);
		return error;
	}
	return fd;
}
