#include "platform/nb_platform.h"

#include "nb/mem.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

#include <limits.h>
#include <string.h>

#define NB_POLL_STACK_ITEMS 16u

static INIT_ONCE g_wsa_once = INIT_ONCE_STATIC_INIT;
static int g_wsa_error = 0;

static BOOL CALLBACK wsa_startup_once(PINIT_ONCE once, PVOID parameter,
                                      PVOID *context)
{
    (void)once;
    (void)parameter;
    (void)context;
    WSADATA data;
    int rc = WSAStartup(MAKEWORD(2, 2), &data);
    if (rc == 0 &&
        (LOBYTE(data.wVersion) != 2 || HIBYTE(data.wVersion) != 2)) {
        WSACleanup();
        rc = WSAVERNOTSUPPORTED;
    }
    g_wsa_error = rc;
    return TRUE;
}

static nb_status ensure_winsock(nb_err *err)
{
    if (!InitOnceExecuteOnce(&g_wsa_once, wsa_startup_once, NULL, NULL)) {
        DWORD code = GetLastError();
        nb_err_set_os(err, NB_ERR_OS, (int)code,
                      "InitOnceExecuteOnce failed (win32 error %lu)",
                      (unsigned long)code);
        return NB_ERR_OS;
    }
    if (g_wsa_error != 0) {
        nb_err_set_os(err, NB_ERR_OS, g_wsa_error,
                      "WSAStartup failed (Winsock error %d)", g_wsa_error);
        return NB_ERR_OS;
    }
    return NB_OK;
}

/*
 * WSAPoll only supports Winsock sockets, unlike POSIX poll(2). The fd passed
 * by the caller is therefore the SOCKET value returned by nb_socket_fd(), not
 * a CRT file descriptor.
 */
static SOCKET socket_from_fd(int fd)
{
    return (SOCKET)(uintptr_t)(unsigned int)fd;
}

nb_status nb_platform_poll(nb_poll_item *items, size_t count, int timeout_ms,
                           size_t *ready, nb_err *err)
{
    if (ready) {
        *ready = 0;
    }
    if (!items && count != 0) {
        nb_err_set(err, NB_ERR_INVAL, "items are required");
        return NB_ERR_INVAL;
    }
    if (count == 0) {
        if (timeout_ms < 0) {
            nb_err_set(err, NB_ERR_INVAL, "cannot wait forever with no items");
            return NB_ERR_INVAL;
        }
        if (timeout_ms > 0) {
            Sleep((DWORD)timeout_ms);
        }
        nb_err_set(err, NB_ERR_TIMEOUT, "poll timed out");
        return NB_ERR_TIMEOUT;
    }
    if (count > (size_t)ULONG_MAX ||
        count > (SIZE_MAX / sizeof(WSAPOLLFD))) {
        nb_err_set(err, NB_ERR_RANGE, "too many poll items");
        return NB_ERR_RANGE;
    }
    nb_status status = ensure_winsock(err);
    if (status != NB_OK) {
        return status;
    }

    WSAPOLLFD stack_items[NB_POLL_STACK_ITEMS];
    WSAPOLLFD *fds = stack_items;
    if (count > NB_POLL_STACK_ITEMS) {
        fds = nb_try_alloc(count * sizeof(WSAPOLLFD));
        if (!fds) {
            nb_err_set(err, NB_ERR_NOMEM, "out of memory polling %zu items",
                       count);
            return NB_ERR_NOMEM;
        }
    }

    for (size_t i = 0; i < count; i++) {
        fds[i].fd = socket_from_fd(items[i].fd);
        fds[i].events = 0;
        if (items[i].want_read) {
            fds[i].events |= POLLRDNORM;
        }
        if (items[i].want_write) {
            fds[i].events |= POLLWRNORM;
        }
        fds[i].revents = 0;
    }

    int rc = WSAPoll(fds, (ULONG)count, timeout_ms < 0 ? -1 : timeout_ms);
    if (rc == SOCKET_ERROR) {
        int code = WSAGetLastError();
        if (fds != stack_items) {
            nb_free(fds, count * sizeof(WSAPOLLFD));
        }
        status = (code == WSAENOTSOCK || code == WSAEINVAL)
                     ? NB_ERR_INVAL
                     : NB_ERR_OS;
        nb_err_set_os(err, status, code, "WSAPoll failed (Winsock error %d)",
                      code);
        return status;
    }

    size_t ready_count = 0;
    for (size_t i = 0; i < count; i++) {
        short revents = fds[i].revents;
        if (revents == 0) {
            continue;
        }
        ready_count++;
        items[i].ready_read =
            (revents & (POLLRDNORM | POLLIN | POLLHUP)) != 0;
        items[i].ready_write = (revents & POLLWRNORM) != 0;
        items[i].error = (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
    }
    if (fds != stack_items) {
        nb_free(fds, count * sizeof(WSAPOLLFD));
    }
    if (ready) {
        *ready = ready_count;
    }
    if (ready_count == 0) {
        nb_err_set(err, NB_ERR_TIMEOUT, "poll timed out");
        return NB_ERR_TIMEOUT;
    }
    return NB_OK;
}
