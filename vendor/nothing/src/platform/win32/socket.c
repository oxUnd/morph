#include "nb/socket.h"

#include "nb/mem.h"
#include "nb/time.h"

#include <limits.h>
#include <string.h>

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

/* WSAEAGAIN is an alias for WSAEWOULDBLOCK on Winsock. */
#ifndef WSAEAGAIN
#define WSAEAGAIN WSAEWOULDBLOCK
#endif

struct nb_socket {
    SOCKET fd;
    nb_af family;
    nb_sock_type type;
    bool nonblock;
};

static INIT_ONCE g_wsa_once = INIT_ONCE_STATIC_INIT;
static int g_wsa_error = 0;

static const char *wsa_error_text(int code, char *buf, size_t cap)
{
    DWORD length = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
        (DWORD)code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf,
        (DWORD)cap, NULL);
    if (length == 0) {
        snprintf(buf, cap, "Winsock error %d", code);
        return buf;
    }
    while (length > 0) {
        char c = buf[length - 1u];
        if (c != '\r' && c != '\n' && c != ' ' && c != '\t' && c != '.') {
            break;
        }
        buf[--length] = '\0';
    }
    if (length == 0) {
        snprintf(buf, cap, "Winsock error %d", code);
    }
    return buf;
}

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
                      "InitOnceExecuteOnce failed: %lu",
                      (unsigned long)code);
        return NB_ERR_OS;
    }
    if (g_wsa_error != 0) {
        char text[256];
        nb_err_set_os(err, NB_ERR_OS, g_wsa_error, "WSAStartup: %s",
                      wsa_error_text(g_wsa_error, text, sizeof(text)));
        return NB_ERR_OS;
    }
    return NB_OK;
}

static nb_status map_wsa_error(int code)
{
    if (code == WSAEWOULDBLOCK || code == WSAEAGAIN) {
        return NB_ERR_AGAIN;
    }
    if (code == WSAETIMEDOUT) {
        return NB_ERR_TIMEOUT;
    }
    return NB_ERR_OS;
}

static nb_status socket_error_code(nb_err *err, int code, const char *what)
{
    char text[256];
    nb_status status = map_wsa_error(code);
    nb_err_set_os(err, status, code, "%s: %s", what,
                  wsa_error_text(code, text, sizeof(text)));
    return status;
}

static nb_status socket_error(nb_err *err, const char *what)
{
    return socket_error_code(err, WSAGetLastError(), what);
}

static bool from_sockaddr(const struct sockaddr *sa, int len, nb_addr *out)
{
    if (!sa || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (sa->sa_family == AF_INET && len >= (int)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        out->family = NB_AF_INET;
        out->port = ntohs(in->sin_port);
        memcpy(out->bytes, &in->sin_addr, 4u);
        return true;
    }
    if (sa->sa_family == AF_INET6 &&
        len >= (int)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        out->family = NB_AF_INET6;
        out->port = ntohs(in6->sin6_port);
        out->scope_id = in6->sin6_scope_id;
        memcpy(out->bytes, &in6->sin6_addr, 16u);
        return true;
    }
    return false;
}

static bool to_sockaddr(const nb_addr *addr, struct sockaddr_storage *storage,
                        int *len)
{
    if (!addr || !storage || !len) {
        return false;
    }
    memset(storage, 0, sizeof(*storage));
    if (addr->family == NB_AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)(void *)storage;
        in->sin_family = AF_INET;
        in->sin_port = htons(addr->port);
        memcpy(&in->sin_addr, addr->bytes, 4u);
        *len = (int)sizeof(*in);
        return true;
    }
    if (addr->family == NB_AF_INET6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)(void *)storage;
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(addr->port);
        in6->sin6_scope_id = addr->scope_id;
        memcpy(&in6->sin6_addr, addr->bytes, 16u);
        *len = (int)sizeof(*in6);
        return true;
    }
    return false;
}

/* FIONBIO takes the non-blocking flag, so blocking means mode == 0. */
static nb_status set_blocking_mode(nb_socket *sock, bool blocking, nb_err *err)
{
    u_long mode = blocking ? 0u : 1u;
    if (ioctlsocket(sock->fd, FIONBIO, &mode) != 0) {
        return socket_error(err, "ioctlsocket(FIONBIO)");
    }
    sock->nonblock = !blocking;
    return NB_OK;
}

nb_status nb_addr_parse(const char *ip, uint16_t port, nb_addr *out,
                        nb_err *err)
{
    if (!ip || !out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid address arguments");
        return NB_ERR_INVAL;
    }
    nb_status status = ensure_winsock(err);
    if (status != NB_OK) {
        return status;
    }

    char buffer[128];
    const char *host = ip;
    size_t len = strlen(ip);
    if (len >= 2u && ip[0] == '[' && ip[len - 1u] == ']') {
        if (len - 2u >= sizeof(buffer)) {
            nb_err_set(err, NB_ERR_RANGE, "address is too long");
            return NB_ERR_RANGE;
        }
        memcpy(buffer, ip + 1u, len - 2u);
        buffer[len - 2u] = '\0';
        host = buffer;
    } else if (len >= sizeof(buffer)) {
        nb_err_set(err, NB_ERR_RANGE, "address is too long");
        return NB_ERR_RANGE;
    }

    memset(out, 0, sizeof(*out));
    out->port = port;
    if (inet_pton(AF_INET, host, out->bytes) == 1) {
        out->family = NB_AF_INET;
        return NB_OK;
    }
    if (inet_pton(AF_INET6, host, out->bytes) == 1) {
        out->family = NB_AF_INET6;
        return NB_OK;
    }
    nb_err_set(err, NB_ERR_INVAL, "invalid IP address '%s'", ip);
    return NB_ERR_INVAL;
}

nb_status nb_addr_format(const nb_addr *addr, char *buf, size_t cap,
                         bool with_port)
{
    if (!addr || !buf || cap == 0) {
        return NB_ERR_INVAL;
    }
    if (ensure_winsock(NULL) != NB_OK) {
        return NB_ERR_OS;
    }

    char text[INET6_ADDRSTRLEN];
    int family = addr->family == NB_AF_INET ? AF_INET :
                 addr->family == NB_AF_INET6 ? AF_INET6 : 0;
    if (family == 0 || !inet_ntop(family, addr->bytes, text, sizeof(text))) {
        return NB_ERR_INVAL;
    }
    int written;
    if (!with_port) {
        written = snprintf(buf, cap, "%s", text);
    } else if (addr->family == NB_AF_INET6) {
        written = snprintf(buf, cap, "[%s]:%u", text, (unsigned)addr->port);
    } else {
        written = snprintf(buf, cap, "%s:%u", text, (unsigned)addr->port);
    }
    if (written < 0 || (size_t)written >= cap) {
        return NB_ERR_RANGE;
    }
    return NB_OK;
}

nb_status nb_dns_resolve(const char *host, uint16_t port, nb_addr *out,
                         size_t max, size_t *count, nb_err *err)
{
    if (!host || !out || max == 0) {
        nb_err_set(err, NB_ERR_INVAL, "invalid DNS arguments");
        return NB_ERR_INVAL;
    }
    if (count) {
        *count = 0;
    }
    nb_status status = ensure_winsock(err);
    if (status != NB_OK) {
        return status;
    }

    char service[8];
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = 0;
    struct addrinfo *result = NULL;
    int rc = getaddrinfo(host, service, &hints, &result);
    if (rc != 0) {
        const char *message = gai_strerrorA(rc);
        nb_err_set_os(err, NB_ERR_OS, rc, "getaddrinfo '%s': %s", host,
                      message ? message : "unknown error");
        return NB_ERR_OS;
    }

    size_t n = 0;
    for (struct addrinfo *ai = result; ai && n < max; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
            continue;
        }
        if (!ai->ai_addr || ai->ai_addrlen > (size_t)INT_MAX) {
            continue;
        }
        if (from_sockaddr(ai->ai_addr, (int)ai->ai_addrlen, &out[n])) {
            n++;
        }
    }
    freeaddrinfo(result);
    if (n == 0) {
        nb_err_set(err, NB_ERR_NOT_FOUND, "no addresses for '%s'", host);
        return NB_ERR_NOT_FOUND;
    }
    if (count) {
        *count = n;
    }
    return NB_OK;
}

bool nb_addr_equal(const nb_addr *a, const nb_addr *b)
{
    if (!a || !b || a->family != b->family || a->port != b->port ||
        a->scope_id != b->scope_id) {
        return false;
    }
    size_t length = a->family == NB_AF_INET ? 4u :
                    a->family == NB_AF_INET6 ? 16u : 0u;
    return length != 0 && memcmp(a->bytes, b->bytes, length) == 0;
}

nb_status nb_socket_open(nb_socket **out, nb_af family, nb_sock_type type,
                         nb_err *err)
{
    if (!out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid socket output");
        return NB_ERR_INVAL;
    }
    int os_family = family == NB_AF_INET ? AF_INET :
                    family == NB_AF_INET6 ? AF_INET6 : 0;
    int os_type = type == NB_SOCK_STREAM ? SOCK_STREAM :
                  type == NB_SOCK_DGRAM ? SOCK_DGRAM : 0;
    if (os_family == 0 || os_type == 0) {
        nb_err_set(err, NB_ERR_INVAL, "invalid socket family or type");
        return NB_ERR_INVAL;
    }
    nb_status status = ensure_winsock(err);
    if (status != NB_OK) {
        return status;
    }

    SOCKET fd = socket(os_family, os_type, 0);
    if (fd == INVALID_SOCKET) {
        return socket_error(err, "socket");
    }
    nb_socket *sock = nb_try_alloc(sizeof(*sock));
    if (!sock) {
        closesocket(fd);
        nb_err_set(err, NB_ERR_NOMEM, "out of memory creating socket");
        return NB_ERR_NOMEM;
    }
    sock->fd = fd;
    sock->family = family;
    sock->type = type;
    sock->nonblock = false;
    *out = sock;
    return NB_OK;
}

void nb_socket_free(nb_socket *sock)
{
    if (!sock) {
        return;
    }
    if (sock->fd != INVALID_SOCKET) {
        closesocket(sock->fd);
    }
    nb_free(sock, sizeof(*sock));
}

int nb_socket_fd(const nb_socket *sock)
{
    return sock ? (int)sock->fd : -1;
}

nb_status nb_socket_set_nonblock(nb_socket *sock, bool enabled, nb_err *err)
{
    if (!sock) {
        nb_err_set(err, NB_ERR_INVAL, "invalid socket");
        return NB_ERR_INVAL;
    }
    return set_blocking_mode(sock, !enabled, err);
}

static nb_status set_boolean_option(nb_socket *sock, int level, int option,
                                    bool enabled, const char *what,
                                    nb_err *err)
{
    int value = enabled ? 1 : 0;
    if (setsockopt(sock->fd, level, option, (const char *)&value,
                   (int)sizeof(value)) != 0) {
        return socket_error(err, what);
    }
    return NB_OK;
}

nb_status nb_socket_set_reuseaddr(nb_socket *sock, bool enabled, nb_err *err)
{
    if (!sock) {
        nb_err_set(err, NB_ERR_INVAL, "invalid socket");
        return NB_ERR_INVAL;
    }
    return set_boolean_option(sock, SOL_SOCKET, SO_REUSEADDR, enabled,
                              "setsockopt(SO_REUSEADDR)", err);
}

nb_status nb_socket_set_nodelay(nb_socket *sock, bool enabled, nb_err *err)
{
    if (!sock || sock->type != NB_SOCK_STREAM) {
        nb_err_set(err, NB_ERR_INVAL,
                   "TCP_NODELAY requires a stream socket");
        return NB_ERR_INVAL;
    }
    return set_boolean_option(sock, IPPROTO_TCP, TCP_NODELAY, enabled,
                              "setsockopt(TCP_NODELAY)", err);
}

nb_status nb_socket_set_keepalive(nb_socket *sock, bool enabled, nb_err *err)
{
    if (!sock) {
        nb_err_set(err, NB_ERR_INVAL, "invalid socket");
        return NB_ERR_INVAL;
    }
    return set_boolean_option(sock, SOL_SOCKET, SO_KEEPALIVE, enabled,
                              "setsockopt(SO_KEEPALIVE)", err);
}

static nb_status set_timeout_option(nb_socket *sock, int option,
                                    int timeout_ms, const char *what,
                                    nb_err *err)
{
    DWORD value = timeout_ms > 0 ? (DWORD)timeout_ms : 0u;
    if (setsockopt(sock->fd, SOL_SOCKET, option, (const char *)&value,
                   (int)sizeof(value)) != 0) {
        return socket_error(err, what);
    }
    return NB_OK;
}

nb_status nb_socket_set_recv_timeout(nb_socket *sock, int timeout_ms,
                                     nb_err *err)
{
    if (!sock) {
        nb_err_set(err, NB_ERR_INVAL, "invalid socket");
        return NB_ERR_INVAL;
    }
    return set_timeout_option(sock, SO_RCVTIMEO, timeout_ms,
                              "setsockopt(SO_RCVTIMEO)", err);
}

nb_status nb_socket_set_send_timeout(nb_socket *sock, int timeout_ms,
                                     nb_err *err)
{
    if (!sock) {
        nb_err_set(err, NB_ERR_INVAL, "invalid socket");
        return NB_ERR_INVAL;
    }
    return set_timeout_option(sock, SO_SNDTIMEO, timeout_ms,
                              "setsockopt(SO_SNDTIMEO)", err);
}

nb_status nb_socket_bind(nb_socket *sock, const nb_addr *addr, nb_err *err)
{
    if (!sock || !addr) {
        nb_err_set(err, NB_ERR_INVAL, "invalid socket or address");
        return NB_ERR_INVAL;
    }
    struct sockaddr_storage storage;
    int length;
    if (!to_sockaddr(addr, &storage, &length)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid address family");
        return NB_ERR_INVAL;
    }
    if (bind(sock->fd, (const struct sockaddr *)&storage, length) != 0) {
        return socket_error(err, "bind");
    }
    return NB_OK;
}

nb_status nb_socket_listen(nb_socket *sock, int backlog, nb_err *err)
{
    if (!sock) {
        nb_err_set(err, NB_ERR_INVAL, "invalid socket");
        return NB_ERR_INVAL;
    }
    if (backlog <= 0) {
        backlog = SOMAXCONN;
    }
    if (listen(sock->fd, backlog) != 0) {
        return socket_error(err, "listen");
    }
    return NB_OK;
}

nb_status nb_socket_accept(nb_socket *sock, nb_socket **out, nb_addr *peer,
                           nb_err *err)
{
    if (!sock || !out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid socket or output");
        return NB_ERR_INVAL;
    }
    for (;;) {
        struct sockaddr_storage storage;
        int length = (int)sizeof(storage);
        SOCKET fd = accept(sock->fd, (struct sockaddr *)&storage, &length);
        if (fd == INVALID_SOCKET) {
            int code = WSAGetLastError();
            if (code == WSAEINTR) {
                continue;
            }
            if (code == WSAEWOULDBLOCK || code == WSAEAGAIN) {
                return NB_ERR_AGAIN;
            }
            return socket_error_code(err, code, "accept");
        }

        nb_socket *client = nb_try_alloc(sizeof(*client));
        if (!client) {
            closesocket(fd);
            nb_err_set(err, NB_ERR_NOMEM,
                       "out of memory accepting socket");
            return NB_ERR_NOMEM;
        }
        client->fd = fd;
        client->family = sock->family;
        client->type = sock->type;
        client->nonblock = false;
        if (sock->nonblock) {
            nb_status status = nb_socket_set_nonblock(client, true, err);
            if (status != NB_OK) {
                closesocket(fd);
                nb_free(client, sizeof(*client));
                return status;
            }
        }
        if (peer && !from_sockaddr((const struct sockaddr *)&storage, length,
                                   peer)) {
            closesocket(fd);
            nb_free(client, sizeof(*client));
            nb_err_set(err, NB_ERR_OS, "cannot read peer address");
            return NB_ERR_OS;
        }
        *out = client;
        return NB_OK;
    }
}

typedef enum connect_wait_result {
    CONNECT_WAIT_READY,
    CONNECT_WAIT_TIMEOUT,
    CONNECT_WAIT_ERROR
} connect_wait_result;

static connect_wait_result wait_connect_ready(const nb_socket *sock,
                                              int timeout_ms,
                                              int *os_error)
{
    *os_error = 0;
    int64_t deadline = nb_time_monotonic_ns() +
                       (int64_t)timeout_ms * INT64_C(1000000);
    for (;;) {
        int64_t remaining = deadline - nb_time_monotonic_ns();
        if (remaining <= 0) {
            return CONNECT_WAIT_TIMEOUT;
        }
        int64_t ms = (remaining + INT64_C(999999)) / INT64_C(1000000);
        if (ms > (int64_t)INT_MAX) {
            ms = INT_MAX;
        }
        struct timeval timeout;
        timeout.tv_sec = (long)(ms / 1000);
        timeout.tv_usec = (long)((ms % 1000) * 1000);
        /*
         * Winsock reports a completed non-blocking connect in writefds and
         * a failed one in exceptfds. Wait on both, then read SO_ERROR below
         * to distinguish success from failure.
         */
        fd_set writable;
        fd_set failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(sock->fd, &writable);
        FD_SET(sock->fd, &failed);
        int result = select(0, NULL, &writable, &failed, &timeout);
        if (result == SOCKET_ERROR) {
            int code = WSAGetLastError();
            if (code == WSAEINTR) {
                continue;
            }
            *os_error = code;
            return CONNECT_WAIT_ERROR;
        }
        if (result == 0) {
            return CONNECT_WAIT_TIMEOUT;
        }
        return CONNECT_WAIT_READY;
    }
}

static void restore_blocking(nb_socket *sock, bool was_nonblock)
{
    if (!was_nonblock) {
        (void)set_blocking_mode(sock, true, NULL);
    }
}

nb_status nb_socket_connect(nb_socket *sock, const nb_addr *addr,
                            int timeout_ms, nb_err *err)
{
    if (!sock || !addr || timeout_ms < 0) {
        nb_err_set(err, NB_ERR_INVAL, "invalid connect arguments");
        return NB_ERR_INVAL;
    }
    struct sockaddr_storage storage;
    int length;
    if (!to_sockaddr(addr, &storage, &length)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid address family");
        return NB_ERR_INVAL;
    }

    if (timeout_ms == 0) {
        for (;;) {
            if (connect(sock->fd, (const struct sockaddr *)&storage,
                        length) == 0) {
                return NB_OK;
            }
            int code = WSAGetLastError();
            if (code == WSAEINTR) {
                continue;
            }
            return socket_error_code(err, code, "connect");
        }
    }

    bool was_nonblock = sock->nonblock;
    if (!was_nonblock) {
        nb_status status = set_blocking_mode(sock, false, err);
        if (status != NB_OK) {
            return status;
        }
    }

    int connect_code = 0;
    bool connected = false;
    for (;;) {
        if (connect(sock->fd, (const struct sockaddr *)&storage, length) ==
            0) {
            connected = true;
            break;
        }
        connect_code = WSAGetLastError();
        if (connect_code == WSAEINTR) {
            continue;
        }
        break;
    }

    if (!connected && connect_code != WSAEWOULDBLOCK &&
        connect_code != WSAEINPROGRESS && connect_code != WSAEALREADY) {
        restore_blocking(sock, was_nonblock);
        return socket_error_code(err, connect_code, "connect");
    }

    if (!connected) {
        int wait_error = 0;
        connect_wait_result wait =
            wait_connect_ready(sock, timeout_ms, &wait_error);
        if (wait == CONNECT_WAIT_TIMEOUT) {
            restore_blocking(sock, was_nonblock);
            nb_err_set(err, NB_ERR_TIMEOUT, "connect timed out");
            return NB_ERR_TIMEOUT;
        }
        if (wait == CONNECT_WAIT_ERROR) {
            restore_blocking(sock, was_nonblock);
            return socket_error_code(err, wait_error, "select");
        }

        int so_error = 0;
        int so_length = (int)sizeof(so_error);
        if (getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, (char *)&so_error,
                       &so_length) != 0) {
            int code = WSAGetLastError();
            restore_blocking(sock, was_nonblock);
            return socket_error_code(err, code, "getsockopt(SO_ERROR)");
        }
        if (so_error != 0) {
            restore_blocking(sock, was_nonblock);
            return socket_error_code(err, so_error, "connect");
        }
    }

    if (!was_nonblock) {
        nb_status status = set_blocking_mode(sock, true, err);
        if (status != NB_OK) {
            return status;
        }
    }
    return NB_OK;
}

static bool wsa_would_block(int code)
{
    return code == WSAEWOULDBLOCK || code == WSAEAGAIN;
}

nb_status nb_socket_send(nb_socket *sock, const void *data, size_t len,
                         size_t *sent, nb_err *err)
{
    if (!sock || (!data && len != 0) || !sent) {
        nb_err_set(err, NB_ERR_INVAL, "invalid send arguments");
        return NB_ERR_INVAL;
    }
    *sent = 0;
    int chunk = len > (size_t)INT_MAX ? INT_MAX : (int)len;
    for (;;) {
        int n = send(sock->fd, (const char *)data, chunk, 0);
        if (n == SOCKET_ERROR) {
            int code = WSAGetLastError();
            if (code == WSAEINTR) {
                continue;
            }
            if (wsa_would_block(code)) {
                return NB_ERR_AGAIN;
            }
            return socket_error_code(err, code, "send");
        }
        *sent = (size_t)n;
        return NB_OK;
    }
}

nb_status nb_socket_recv(nb_socket *sock, void *data, size_t len,
                         size_t *received, nb_err *err)
{
    if (!sock || (!data && len != 0) || !received) {
        nb_err_set(err, NB_ERR_INVAL, "invalid receive arguments");
        return NB_ERR_INVAL;
    }
    *received = 0;
    int chunk = len > (size_t)INT_MAX ? INT_MAX : (int)len;
    for (;;) {
        int n = recv(sock->fd, (char *)data, chunk, 0);
        if (n == SOCKET_ERROR) {
            int code = WSAGetLastError();
            if (code == WSAEINTR) {
                continue;
            }
            if (wsa_would_block(code)) {
                return NB_ERR_AGAIN;
            }
            return socket_error_code(err, code, "recv");
        }
        *received = (size_t)n;
        return NB_OK;
    }
}

nb_status nb_socket_send_all(nb_socket *sock, const void *data, size_t len,
                             nb_err *err)
{
    if (!sock || (!data && len != 0)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid send arguments");
        return NB_ERR_INVAL;
    }
    const unsigned char *ptr = (const unsigned char *)data;
    size_t left = len;
    while (left > 0) {
        size_t sent = 0;
        nb_status status = nb_socket_send(sock, ptr, left, &sent, err);
        if (status != NB_OK) {
            return status;
        }
        if (sent == 0) {
            nb_err_set(err, NB_ERR_IO, "send returned zero bytes");
            return NB_ERR_IO;
        }
        ptr += sent;
        left -= sent;
    }
    return NB_OK;
}

nb_status nb_socket_recv_all(nb_socket *sock, void *data, size_t len,
                             nb_err *err)
{
    if (!sock || (!data && len != 0)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid receive arguments");
        return NB_ERR_INVAL;
    }
    unsigned char *ptr = (unsigned char *)data;
    size_t left = len;
    while (left > 0) {
        size_t received = 0;
        nb_status status = nb_socket_recv(sock, ptr, left, &received, err);
        if (status != NB_OK) {
            return status;
        }
        if (received == 0) {
            nb_err_set(err, NB_ERR_IO, "connection closed while receiving");
            return NB_ERR_IO;
        }
        ptr += received;
        left -= received;
    }
    return NB_OK;
}

nb_status nb_socket_shutdown(nb_socket *sock, bool read_side, bool write_side,
                             nb_err *err)
{
    if (!sock || (!read_side && !write_side)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid shutdown arguments");
        return NB_ERR_INVAL;
    }
    int how = read_side && write_side ? SD_BOTH :
              read_side ? SD_RECEIVE : SD_SEND;
    if (shutdown(sock->fd, how) != 0) {
        return socket_error(err, "shutdown");
    }
    return NB_OK;
}

nb_status nb_socket_local_addr(const nb_socket *sock, nb_addr *out, nb_err *err)
{
    if (!sock || !out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid socket or address output");
        return NB_ERR_INVAL;
    }
    struct sockaddr_storage storage;
    int length = (int)sizeof(storage);
    if (getsockname(sock->fd, (struct sockaddr *)&storage, &length) != 0) {
        return socket_error(err, "getsockname");
    }
    if (!from_sockaddr((const struct sockaddr *)&storage, length, out)) {
        nb_err_set(err, NB_ERR_OS, "unsupported local address family");
        return NB_ERR_OS;
    }
    return NB_OK;
}

nb_status nb_socket_peer_addr(const nb_socket *sock, nb_addr *out, nb_err *err)
{
    if (!sock || !out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid socket or address output");
        return NB_ERR_INVAL;
    }
    struct sockaddr_storage storage;
    int length = (int)sizeof(storage);
    if (getpeername(sock->fd, (struct sockaddr *)&storage, &length) != 0) {
        return socket_error(err, "getpeername");
    }
    if (!from_sockaddr((const struct sockaddr *)&storage, length, out)) {
        nb_err_set(err, NB_ERR_OS, "unsupported peer address family");
        return NB_ERR_OS;
    }
    return NB_OK;
}
