#include "platform/nb_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <errno.h>
#include <fcntl.h>
#include "nb/thread.h"
#include <io.h>
#include <limits.h>
#include <string.h>

static nb_status map_read_errno(int saved, nb_err *err)
{
    nb_status status = NB_ERR_IO;
    if (saved == EBADF) {
        status = NB_ERR_INVAL;
    } else if (saved == EAGAIN) {
        status = NB_ERR_AGAIN;
    }
    nb_err_set_os(err, status, saved, "_read failed: %s", strerror(saved));
    return status;
}

nb_status nb_platform_fd_read(int fd, void *data, size_t len, size_t *read,
                              nb_err *err)
{
    if (read) {
        *read = 0;
    }
    if (fd < 0 || !read || (!data && len != 0)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid fd, buffer or output");
        return NB_ERR_INVAL;
    }
    if (len == 0) {
        return NB_OK;
    }

    intptr_t os_handle = _get_osfhandle(fd);
    if (os_handle == -1) {
        nb_err_set_os(err, NB_ERR_INVAL, EBADF, "invalid CRT fd %d", fd);
        return NB_ERR_INVAL;
    }
    HANDLE handle = (HANDLE)os_handle;
    /* Anonymous pipes signal readiness for reads; files are always ready. */
    if (GetFileType(handle) == FILE_TYPE_PIPE) {
        DWORD wait = WaitForSingleObject(handle, INFINITE);
        if (wait == WAIT_FAILED) {
            DWORD code = GetLastError();
            nb_err_set_os(err, NB_ERR_OS, (int)code,
                          "WaitForSingleObject failed (win32 error %lu)",
                          (unsigned long)code);
            return NB_ERR_OS;
        }
    }

    unsigned int chunk = len > (size_t)INT_MAX ? (unsigned int)INT_MAX
                                               : (unsigned int)len;
    int n;
    do {
        n = _read(fd, data, chunk);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        return map_read_errno(errno, err);
    }
    *read = (size_t)n;
    return NB_OK;
}

nb_status nb_platform_pipe_open(int descriptors[2], nb_err *err)
{
    int fds[2];
    if (_pipe(fds, 4096, _O_BINARY | _O_NOINHERIT) != 0)
        return nb_status_from_errno(errno, err);
    descriptors[0] = fds[0];
    descriptors[1] = fds[1];
    return NB_OK;
}
struct bounded_writer {
    HANDLE output;
    const unsigned char *data;
    size_t len;
    nb_atomic_int stop;
    nb_status status;
    DWORD error;
};

static DWORD WINAPI write_thread(void *opaque)
{
    struct bounded_writer *writer = opaque;
    size_t offset = 0;
    writer->status = NB_OK;
    while (offset < writer->len && !nb_atomic_int_load(&writer->stop)) {
        size_t left = writer->len - offset;
        DWORD chunk = left > 65536u ? 65536u : (DWORD)left;
        DWORD written = 0;
        if (!WriteFile(writer->output, writer->data + offset, chunk, &written, NULL)) {
            writer->error = GetLastError();
            writer->status = writer->error == ERROR_BROKEN_PIPE ? NB_ERR_BROKEN_PIPE : NB_ERR_IO;
            return 0;
        }
        if (!written) {
            writer->status = NB_ERR_BROKEN_PIPE;
            return 0;
        }
        offset += (size_t)written;
    }
    return 0;
}

nb_status nb_platform_fd_write_until(int fd, const void *data, size_t len, int64_t deadline_ns,
                                     int (*cancelled)(void *), void *ctx, nb_err *err)
{
    intptr_t native = _get_osfhandle(fd);
    if (native == -1)
        return nb_status_from_errno(EBADF, err);
    if (cancelled && cancelled(ctx))
        return NB_ERR_CANCELLED;
    if (!len)
        return NB_OK;
    struct bounded_writer writer = {
        .output = (HANDLE)native, .data = data, .len = len, .status = NB_OK};
    nb_atomic_int_init(&writer.stop, 0);
    HANDLE thread = CreateThread(NULL, 0, write_thread, &writer, 0, NULL);
    if (!thread) {
        nb_err_set_os(err, NB_ERR_OS, (int)GetLastError(), "CreateThread failed");
        return NB_ERR_OS;
    }
    nb_status status = NB_OK;
    for (;;) {
        DWORD waited = WaitForSingleObject(thread, 10);
        if (waited == WAIT_OBJECT_0)
            break;
        if (waited == WAIT_FAILED) {
            status = NB_ERR_OS;
            break;
        }
        if (cancelled && cancelled(ctx)) {
            status = NB_ERR_CANCELLED;
            break;
        }
        if (nb_platform_time_monotonic_ns() >= deadline_ns) {
            status = NB_ERR_TIMEOUT;
            break;
        }
    }
    if (status != NB_OK) {
        nb_atomic_int_store(&writer.stop, 1);
        /* Repeat cancellation to cover the race between the worker's stop
         * check and entering its next synchronous WriteFile call. */
        do {
            (void)CancelSynchronousIo(thread);
        } while (WaitForSingleObject(thread, 10) == WAIT_TIMEOUT);
    } else {
        status = writer.status;
    }
    CloseHandle(thread);
    if (status != NB_OK) {
        nb_err_set_os(err, status, (int)writer.error, "%s", nb_status_str(status));
    }
    return status;
}
