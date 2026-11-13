#include "nb/socket.h"

#include "nb/mem.h"
#include "nb/time.h"

#include <errno.h>
#include <limits.h>
#include <string.h>


#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

struct nb_socket {
    int fd;
    nb_af family;
    nb_sock_type type;
    bool nonblock;
};

static nb_status socket_error(nb_err *err, nb_status status, const char *what)
{
    int saved = errno;
    nb_err_set_os(err, status, saved, "%s: %s", what, strerror(saved));
    return status;
}

static bool from_sockaddr(const struct sockaddr *sa, socklen_t len,
                          nb_addr *out)
{
    if (!sa || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (sa->sa_family == AF_INET && len >= (socklen_t)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        out->family = NB_AF_INET;
        out->port = ntohs(in->sin_port);
        memcpy(out->bytes, &in->sin_addr, 4u);
        return true;
    }
    if (sa->sa_family == AF_INET6 && len >= (socklen_t)sizeof(struct sockaddr_in6)) {
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
                        socklen_t *len)
{
    if (!addr || !storage || !len) {
        return false;
    }
    memset(storage, 0, sizeof(*storage));
    if (addr->family == NB_AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)storage;
        in->sin_family = AF_INET;
        in->sin_port = htons(addr->port);
        memcpy(&in->sin_addr, addr->bytes, 4u);
        *len = (socklen_t)sizeof(*in);
        return true;
    }
    if (addr->family == NB_AF_INET6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)storage;
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(addr->port);
        in6->sin6_scope_id = addr->scope_id;
        memcpy(&in6->sin6_addr, addr->bytes, 16u);
        *len = (socklen_t)sizeof(*in6);
        return true;
    }
    return false;
}

static nb_status configure_socket(int fd, nb_err *err)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        return socket_error(err, NB_ERR_OS, "fcntl(FD_CLOEXEC)");
    }
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        return socket_error(err, NB_ERR_OS, "setsockopt(SO_NOSIGPIPE)");
    }
#endif
    return NB_OK;
}

nb_status nb_addr_parse(const char *ip, uint16_t port, nb_addr *out, nb_err *err)
{
    if (!ip || !out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid address arguments");
        return NB_ERR_INVAL;
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
    char service[8];
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = 0;
    struct addrinfo *result = NULL;
    int rc = getaddrinfo(host, service, &hints, &result);
    if (rc != 0) {
        nb_err_set(err, NB_ERR_OS, "getaddrinfo '%s': %s", host,
                   gai_strerror(rc));
        return NB_ERR_OS;
    }
    size_t n = 0;
    for (struct addrinfo *ai = result; ai && n < max; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
            continue;
        }
        if (from_sockaddr(ai->ai_addr, (socklen_t)ai->ai_addrlen, &out[n])) {
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
#ifdef SOCK_CLOEXEC
    int fd = socket(os_family, os_type | SOCK_CLOEXEC, 0);
    if (fd < 0 && (errno == EINVAL || errno == EPROTONOSUPPORT)) {
        fd = socket(os_family, os_type, 0);
    }
#else
    int fd = socket(os_family, os_type, 0);
#endif
    if (fd < 0) {
        return socket_error(err, NB_ERR_OS, "socket");
    }
    nb_status status = configure_socket(fd, err);
    if (status != NB_OK) {
        close(fd);
        return status;
    }
    nb_socket *sock = nb_try_alloc(sizeof(*sock));
    if (!sock) {
        close(fd);
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
    if (sock->fd >= 0) {
        close(sock->fd);
    }
    nb_free(sock, sizeof(*sock));
}

int nb_socket_fd(const nb_socket *sock)
{
    return sock ? sock->fd : -1;
}

nb_status nb_socket_set_nonblock(nb_socket *sock, bool enabled, nb_err *err)
{
    if (!sock) {
        nb_err_set(err, NB_ERR_INVAL, "invalid socket");
        return NB_ERR_INVAL;
    }
    int flags = fcntl(sock->fd, F_GETFL);
    if (flags < 0) {
        return socket_error(err, NB_ERR_OS, "fcntl(F_GETFL)");
    }
    if (enabled) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    if (fcntl(sock->fd, F_SETFL, flags) != 0) {
        return socket_error(err, NB_ERR_OS, "fcntl(F_SETFL)");
    }
    sock->nonblock = enabled;
    return NB_OK;
}

static nb_status set_boolean_option(nb_socket *sock, int level, int option,
                                    bool enabled, const char *what,
                                    nb_err *err)
{
    int value = enabled ? 1 : 0;
    if (setsockopt(sock->fd, level, option, &value, sizeof(value)) != 0) {
        return socket_error(err, NB_ERR_OS, what);
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
        nb_err_set(err, NB_ERR_INVAL, "TCP_NODELAY requires a stream socket");
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

static nb_status set_timeval_option(nb_socket *sock, int option,
                                    int timeout_ms, const char *what,
                                    nb_err *err)
{
    struct timeval tv;
    tv.tv_sec = timeout_ms > 0 ? timeout_ms / 1000 : 0;
    tv.tv_usec = timeout_ms > 0 ? (timeout_ms % 1000) * 1000 : 0;
    if (setsockopt(sock->fd, SOL_SOCKET, option, &tv, sizeof(tv)) != 0) {
        return socket_error(err, NB_ERR_OS, what);
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
    return set_timeval_option(sock, SO_RCVTIMEO, timeout_ms,
                              "setsockopt(SO_RCVTIMEO)", err);
}

nb_status nb_socket_set_send_timeout(nb_socket *sock, int timeout_ms,
                                     nb_err *err)
{
    if (!sock) {
        nb_err_set(err, NB_ERR_INVAL, "invalid socket");
        return NB_ERR_INVAL;
    }
    return set_timeval_option(sock, SO_SNDTIMEO, timeout_ms,
                              "setsockopt(SO_SNDTIMEO)", err);
}

nb_status nb_socket_bind(nb_socket *sock, const nb_addr *addr, nb_err *err)
{
    if (!sock || !addr) {
        nb_err_set(err, NB_ERR_INVAL, "invalid socket or address");
        return NB_ERR_INVAL;
    }
    struct sockaddr_storage storage;
    socklen_t length;
    if (!to_sockaddr(addr, &storage, &length)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid address family");
        return NB_ERR_INVAL;
    }
    if (bind(sock->fd, (const struct sockaddr *)&storage, length) != 0) {
        return socket_error(err, NB_ERR_OS, "bind");
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
        return socket_error(err, NB_ERR_OS, "listen");
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
        socklen_t length = (socklen_t)sizeof(storage);
        int fd = accept(sock->fd, (struct sockaddr *)&storage, &length);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return NB_ERR_AGAIN;
            }
            return socket_error(err, NB_ERR_OS, "accept");
        }
        nb_status status = configure_socket(fd, err);
        if (status != NB_OK) {
            close(fd);
            return status;
        }
        nb_socket *client = nb_try_alloc(sizeof(*client));
        if (!client) {
            close(fd);
            nb_err_set(err, NB_ERR_NOMEM, "out of memory accepting socket");
            return NB_ERR_NOMEM;
        }
        client->fd = fd;
        client->family = sock->family;
        client->type = sock->type;
        client->nonblock = false;
        if (sock->nonblock &&
            nb_socket_set_nonblock(client, true, err) != NB_OK) {
            close(fd);
            nb_free(client, sizeof(*client));
            return err ? err->code : NB_ERR_OS;
        }
        if (peer && !from_sockaddr((const struct sockaddr *)&storage, length,
                                   peer)) {
            close(fd);
            nb_free(client, sizeof(*client));
            nb_err_set(err, NB_ERR_OS, "cannot read peer address");
            return NB_ERR_OS;
        }
        *out = client;
        return NB_OK;
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
    socklen_t length;
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
            if (errno == EINTR) {
                continue;
            }
            return socket_error(err, NB_ERR_OS, "connect");
        }
    }

    int flags = fcntl(sock->fd, F_GETFL);
    if (flags < 0) {
        return socket_error(err, NB_ERR_OS, "fcntl(F_GETFL)");
    }
    bool restore_blocking = (flags & O_NONBLOCK) == 0;
    if (restore_blocking && fcntl(sock->fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return socket_error(err, NB_ERR_OS, "fcntl(F_SETFL)");
    }

    int rc = connect(sock->fd, (const struct sockaddr *)&storage, length);
    if (rc != 0 && errno != EINPROGRESS && errno != EINTR) {
        int saved = errno;
        if (restore_blocking) {
            (void)fcntl(sock->fd, F_SETFL, flags);
        }
        errno = saved;
        return socket_error(err, NB_ERR_OS, "connect");
    }

    if (rc != 0) {
        int64_t deadline = nb_time_monotonic_ns() +
                           (int64_t)timeout_ms * INT64_C(1000000);
        for (;;) {
            int64_t remaining = deadline - nb_time_monotonic_ns();
            int wait_ms = 0;
            if (remaining > 0) {
                int64_t ms = (remaining + INT64_C(999999)) / INT64_C(1000000);
                wait_ms = ms > INT_MAX ? INT_MAX : (int)ms;
            }
            struct pollfd pfd;
            pfd.fd = sock->fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int result = poll(&pfd, 1, wait_ms);
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result == 0) {
                if (restore_blocking) {
                    (void)fcntl(sock->fd, F_SETFL, flags);
                }
                nb_err_set(err, NB_ERR_TIMEOUT, "connect timed out");
                return NB_ERR_TIMEOUT;
            }
            if (result < 0) {
                int saved = errno;
                if (restore_blocking) {
                    (void)fcntl(sock->fd, F_SETFL, flags);
                }
                errno = saved;
                return socket_error(err, NB_ERR_OS, "poll");
            }
            break;
        }
        int so_error = 0;
        socklen_t so_length = (socklen_t)sizeof(so_error);
        if (getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &so_error,
                       &so_length) != 0) {
            int saved = errno;
            if (restore_blocking) {
                (void)fcntl(sock->fd, F_SETFL, flags);
            }
            errno = saved;
            return socket_error(err, NB_ERR_OS, "getsockopt(SO_ERROR)");
        }
        if (so_error != 0) {
            if (restore_blocking) {
                (void)fcntl(sock->fd, F_SETFL, flags);
            }
            errno = so_error;
            return socket_error(err, NB_ERR_OS, "connect");
        }
    }

    if (restore_blocking && fcntl(sock->fd, F_SETFL, flags) != 0) {
        return socket_error(err, NB_ERR_OS, "fcntl(F_SETFL)");
    }
    sock->nonblock = !restore_blocking;
    return NB_OK;
}

nb_status nb_socket_send(nb_socket *sock, const void *data, size_t len,
                         size_t *sent, nb_err *err)
{
    if (!sock || (!data && len != 0) || !sent) {
        nb_err_set(err, NB_ERR_INVAL, "invalid send arguments");
        return NB_ERR_INVAL;
    }
    *sent = 0;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    for (;;) {
        ssize_t n = send(sock->fd, data, len, flags);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return NB_ERR_AGAIN;
            }
            return socket_error(err, NB_ERR_OS, "send");
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
    for (;;) {
        ssize_t n = recv(sock->fd, data, len, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return NB_ERR_AGAIN;
            }
            return socket_error(err, NB_ERR_OS, "recv");
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
    int how = read_side && write_side ? SHUT_RDWR :
              read_side ? SHUT_RD : SHUT_WR;
    if (shutdown(sock->fd, how) != 0) {
        return socket_error(err, NB_ERR_OS, "shutdown");
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
    socklen_t length = (socklen_t)sizeof(storage);
    if (getsockname(sock->fd, (struct sockaddr *)&storage, &length) != 0) {
        return socket_error(err, NB_ERR_OS, "getsockname");
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
    socklen_t length = (socklen_t)sizeof(storage);
    if (getpeername(sock->fd, (struct sockaddr *)&storage, &length) != 0) {
        return socket_error(err, NB_ERR_OS, "getpeername");
    }
    if (!from_sockaddr((const struct sockaddr *)&storage, length, out)) {
        nb_err_set(err, NB_ERR_OS, "unsupported peer address family");
        return NB_ERR_OS;
    }
    return NB_OK;
}
