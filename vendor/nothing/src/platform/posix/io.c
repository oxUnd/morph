#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "platform/nb_platform.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

nb_status nb_platform_fd_read(int fd, void *data, size_t len,
                              size_t *read_out, nb_err *err)
{
    if (read_out) {
        *read_out = 0;
    }
    if (fd < 0 || !read_out || (!data && len != 0)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid fd, buffer or output");
        return NB_ERR_INVAL;
    }
    if (len == 0) {
        return NB_OK;
    }
    if (len > (size_t)SSIZE_MAX) {
        len = (size_t)SSIZE_MAX;
    }

    ssize_t n;
    do {
        n = read(fd, data, len);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        int saved = errno;
        if (saved == EAGAIN || saved == EWOULDBLOCK) {
            nb_err_set_os(err, NB_ERR_AGAIN, saved, "read would block");
            return NB_ERR_AGAIN;
        }
        nb_status status = saved == EBADF ? NB_ERR_INVAL : NB_ERR_IO;
        nb_err_set_os(err, status, saved, "read failed: %s",
                      strerror(saved));
        return status;
    }
    *read_out = (size_t)n;
    return NB_OK;
}

nb_status nb_platform_pipe_open(int descriptors[2], nb_err *err)
{
    int fds[2];
#ifdef __linux__
    if (pipe2(fds, O_CLOEXEC) != 0)
        return nb_status_from_errno(errno, err);
#else
    if (pipe(fds) != 0)
        return nb_status_from_errno(errno, err);
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0) {
        int saved = errno;
        (void)close(fds[0]);
        (void)close(fds[1]);
        return nb_status_from_errno(saved, err);
    }
#endif
    descriptors[0] = fds[0];
    descriptors[1] = fds[1];
    return NB_OK;
}
