#define _GNU_SOURCE
#include "child_io.h"
#include "error.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/**
 * \brief Create an owned pipe that does not leak into unrelated exec children.
 * \param[out] descriptors: Two owned fds on success, both -1 on failure.
 * \return Zero on success, negative errno on failure.
 */
int morph_io_pipe(int descriptors[2])
{
	int fds[2];
	if (descriptors == NULL) {
		return -EINVAL;
	}
	descriptors[0] = -1;
	descriptors[1] = -1;
#ifdef __linux__
	if (pipe2(fds, O_CLOEXEC) != 0) {
		return -errno;
	}
#else
	if (pipe(fds) != 0) {
		return -errno;
	}
	if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0) {
		int rc = -errno;
		close(fds[0]);
		close(fds[1]);
		return rc;
	}
#endif
	descriptors[0] = fds[0];
	descriptors[1] = fds[1];
	return 0;
}

/**
 * \brief Read the monotonic interval clock.
 * \return Process-local monotonic milliseconds.
 */
int64_t morph_io_now_ms(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/** \brief Write without delivering a generated SIGPIPE to the host. */
static ssize_t write_no_signal(int fd, const char *data, size_t len)
{
	sigset_t blocked, previous, pending;
	ssize_t result;
	int saved, already, rc;

	sigemptyset(&blocked);
	sigaddset(&blocked, SIGPIPE);
	rc = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
	if (rc != 0) {
		errno = rc;
		return -1;
	}
	sigpending(&pending);
	already = sigismember(&pending, SIGPIPE);
	result = write(fd, data, len);
	saved = errno;
	if (result < 0 && saved == EPIPE && !already) {
		sigpending(&pending);
		if (sigismember(&pending, SIGPIPE)) {
			int number;
			(void)sigwait(&blocked, &number);
		}
	}
	(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
	errno = saved;
	return result;
}

/**
 * \brief Send bounded data with cancellation, restoring the borrowed fd.
 * \param[in] fd: Exclusively borrowed nonblocking-capable descriptor.
 * \param[in] data: Borrowed bytes, nullable only for an empty request.
 * \param[in] len: Input byte count.
 * \param[in] deadline: Absolute monotonic milliseconds.
 * \param[in] cancelled: Optional synchronous cancellation predicate.
 * \param[in] user: Borrowed predicate context.
 * \return Zero on success, negative errno on cancellation or I/O failure.
 */
int morph_io_write(int fd, const char *data, size_t len, int64_t deadline, int (*cancelled)(void *),
		   void *user)
{
	if (data == NULL && len != 0) {
		return -EINVAL;
	}
	if (cancelled != NULL && cancelled(user)) {
		return -ECANCELED;
	}
	int flags = fcntl(fd, F_GETFL);
	int rc = 0;
	size_t offset = 0;

	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		return -errno;
	}
	while (offset < len) {
		struct pollfd pfd = {.fd = fd, .events = POLLOUT};
		if (cancelled != NULL && cancelled(user)) {
			rc = -ECANCELED;
			break;
		}
		if (morph_io_now_ms() >= deadline) {
			rc = -ETIMEDOUT;
			break;
		}
		int ready = poll(&pfd, 1, 20);
		if (ready < 0 && errno == EINTR) {
			continue;
		}
		if (ready < 0) {
			rc = -errno;
			break;
		}
		if (ready == 0) {
			continue;
		}
		ssize_t count = write_no_signal(fd, data + offset, len - offset);
		if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
			continue;
		}
		if (count <= 0) {
			rc = count == 0 ? -EPIPE : -errno;
			break;
		}
		offset += (size_t)count;
	}
	if (fcntl(fd, F_SETFL, flags) < 0 && rc == 0) {
		rc = -errno;
	}
	return rc;
}

/** \brief Drain one ready nonblocking stream, keeping bounded output. */
static int drain_stream(int fd, morph_buf_t *buffer, size_t limit, int *open)
{
	char bytes[BUFSIZ];
	ssize_t count = read(fd, bytes, sizeof(bytes));
	if (count < 0) {
		return errno == EINTR || errno == EAGAIN ? 0 : -errno;
	}
	if (count == 0) {
		*open = 0;
		return 0;
	}
	size_t keep = buffer->len < limit ? limit - buffer->len : 0;
	if (keep > (size_t)count) {
		keep = (size_t)count;
	}
	int rc = morph_buf_append(buffer, bytes, keep);
	if (rc == 0 && keep < (size_t)count && buffer->len <= limit) {
		rc = morph_buf_puts(buffer, "\n[output truncated at configured limit]\n");
	}
	return rc;
}

/**
 * \brief Communicate over all child streams under a single deadline.
 * \param[in] pid: Owned, unreaped child identity; consumed until reaped.
 * \param[in] input_fd: Owned stdin writer, closed on every valid-call exit.
 * \param[in] output_fd: Exclusively borrowed stdout reader.
 * \param[in] error_fd: Exclusively borrowed stderr reader, or -1.
 * \param[in] input: Borrowed request bytes until return.
 * \param[in] input_len: Request byte count.
 * \param[in] timeout_ms: Whole-operation budget; nonpositive uses five minutes.
 * \param[in] limit: Retained bytes per stream, plus an explicit truncation marker.
 * \param[in] cancelled: Optional synchronous predicate, called without internal locks.
 * \param[in] user: Borrowed predicate context until return.
 * \param[in,out] output: Initialized caller-owned output; remains cleanable on failure.
 * \param[in,out] error: Initialized caller-owned stderr; remains cleanable on failure.
 * \param[out] status: Wait status, initialized to zero even on argument errors.
 * \return Zero after exit and drain, negative errno otherwise; child is reaped.
 */
int morph_child_exchange(pid_t pid, int input_fd, int output_fd, int error_fd, const char *input,
			 size_t input_len, int timeout_ms, size_t limit, int (*cancelled)(void *),
			 void *user, morph_buf_t *output, morph_buf_t *error, int *status)
{
	int fds[] = {input_fd, output_fd, error_fd};
	int flags[] = {-1, -1, -1};
	int open[] = {1, 1, error_fd >= 0};
	int rc = 0, reaped = 0;
	size_t offset = 0;
	int64_t deadline = morph_io_now_ms() + (timeout_ms > 0 ? timeout_ms : 300000);

	if (status != NULL) {
		*status = 0;
	}
	if (status == NULL || pid <= 0 || output == NULL || error == NULL ||
	    (input == NULL && input_len != 0)) {
		return -EINVAL;
	}
	for (int i = 0; i < 3; i++) {
		if (fds[i] < 0) {
			continue;
		}
		flags[i] = fcntl(fds[i], F_GETFL);
		if (flags[i] < 0 || fcntl(fds[i], F_SETFL, flags[i] | O_NONBLOCK) < 0) {
			rc = -errno;
			break;
		}
	}
	while (rc == 0 && (!reaped || open[0] || open[1] || open[2])) {
		struct pollfd pollfds[3];
		if (cancelled != NULL && cancelled(user)) {
			rc = -ECANCELED;
			break;
		}
		if (morph_io_now_ms() >= deadline) {
			rc = -ETIMEDOUT;
			break;
		}
		for (int i = 0; i < 3; i++) {
			pollfds[i].fd = open[i] ? fds[i] : -1;
			pollfds[i].events = i == 0 ? POLLOUT : POLLIN;
			pollfds[i].revents = 0;
		}
		int ready = poll(pollfds, 3, 20);
		if (ready < 0 && errno == EINTR) {
			continue;
		}
		if (ready < 0) {
			rc = -errno;
			break;
		}
		if (open[0] && pollfds[0].revents != 0) {
			ssize_t count = offset < input_len
						? write_no_signal(input_fd, input + offset,
								  input_len - offset)
						: 0;
			if (count > 0) {
				offset += (size_t)count;
			} else if (count < 0 && errno != EINTR && errno != EAGAIN) {
				rc = -errno;
			}
			if (offset == input_len) {
				/* Closing the consumed stdin writer sends protocol EOF. */
				if (close(input_fd) < 0) {
					rc = -errno;
				}
				input_fd = -1;
				fds[0] = -1;
				flags[0] = -1;
				open[0] = 0;
			}
		}
		for (int i = 1; i < 3 && rc == 0; i++) {
			if (open[i] && pollfds[i].revents != 0) {
				rc = drain_stream(fds[i], i == 1 ? output : error, limit, &open[i]);
			}
		}
		if (!reaped) {
			pid_t result = waitpid(pid, status, WNOHANG);
			if (result == pid) {
				reaped = 1;
				if (offset < input_len) {
					rc = -EPIPE;
				}
			} else if (result < 0 && errno != EINTR) {
				rc = -errno;
			}
		}
	}
	if (rc != 0) {
		if (kill(-pid, SIGKILL) < 0 && !reaped) {
			(void)kill(pid, SIGKILL);
		}
	}
	if (rc != 0 && !reaped) {
		while (waitpid(pid, status, 0) < 0) {
			if (errno != EINTR) {
				break;
			}
		}
	}
	if (input_fd >= 0) {
		(void)close(input_fd);
	}
	for (int i = 1; i < 3; i++) {
		if (flags[i] >= 0 && fcntl(fds[i], F_SETFL, flags[i]) < 0 && rc == 0) {
			rc = -errno;
		}
	}
	return rc;
}
