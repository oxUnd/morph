#ifndef NB_SOCKET_H
#define NB_SOCKET_H

#include "nb/base.h"
#include "nb/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nb_af {
    NB_AF_UNSPEC = 0,
    NB_AF_INET = 4,
    NB_AF_INET6 = 6
} nb_af;

typedef enum nb_sock_type {
    NB_SOCK_STREAM = 1,
    NB_SOCK_DGRAM = 2
} nb_sock_type;

typedef struct nb_addr {
    nb_af family;
    uint16_t port;
    uint32_t scope_id;
    unsigned char bytes[16];
} nb_addr;

typedef struct nb_socket nb_socket;

/* Numeric addresses only; use nb_dns_resolve() for host names. */
NB_EXPORT nb_status nb_addr_parse(const char *ip, uint16_t port,
                                  nb_addr *out, nb_err *err);
NB_EXPORT nb_status nb_addr_format(const nb_addr *addr, char *buf, size_t cap,
                                   bool with_port);
NB_EXPORT nb_status nb_dns_resolve(const char *host, uint16_t port,
                                   nb_addr *out, size_t max, size_t *count,
                                   nb_err *err);
NB_EXPORT bool nb_addr_equal(const nb_addr *a, const nb_addr *b);

NB_EXPORT nb_status nb_socket_open(nb_socket **out, nb_af family,
                                   nb_sock_type type, nb_err *err);
NB_EXPORT void nb_socket_free(nb_socket *sock);
NB_EXPORT int nb_socket_fd(const nb_socket *sock);

NB_EXPORT nb_status nb_socket_set_nonblock(nb_socket *sock, bool enabled,
                                           nb_err *err);
NB_EXPORT nb_status nb_socket_set_reuseaddr(nb_socket *sock, bool enabled,
                                            nb_err *err);
NB_EXPORT nb_status nb_socket_set_nodelay(nb_socket *sock, bool enabled,
                                          nb_err *err);
NB_EXPORT nb_status nb_socket_set_keepalive(nb_socket *sock, bool enabled,
                                            nb_err *err);

/* timeout_ms <= 0 clears the timeout. */
NB_EXPORT nb_status nb_socket_set_recv_timeout(nb_socket *sock, int timeout_ms,
                                               nb_err *err);
NB_EXPORT nb_status nb_socket_set_send_timeout(nb_socket *sock, int timeout_ms,
                                               nb_err *err);

NB_EXPORT nb_status nb_socket_bind(nb_socket *sock, const nb_addr *addr,
                                   nb_err *err);
NB_EXPORT nb_status nb_socket_listen(nb_socket *sock, int backlog, nb_err *err);
NB_EXPORT nb_status nb_socket_accept(nb_socket *sock, nb_socket **out,
                                     nb_addr *peer, nb_err *err);

/* timeout_ms == 0 means blocking connect. */
NB_EXPORT nb_status nb_socket_connect(nb_socket *sock, const nb_addr *addr,
                                      int timeout_ms, nb_err *err);

/* Single send/receive operations. A zero length is allowed. Sending on a
 * closed connection reports an error without raising SIGPIPE on Linux/macOS. */
NB_EXPORT nb_status nb_socket_send(nb_socket *sock, const void *data,
                                   size_t len, size_t *sent, nb_err *err);
NB_EXPORT nb_status nb_socket_recv(nb_socket *sock, void *data,
                                   size_t len, size_t *received, nb_err *err);
NB_EXPORT nb_status nb_socket_send_all(nb_socket *sock, const void *data,
                                       size_t len, nb_err *err);
NB_EXPORT nb_status nb_socket_recv_all(nb_socket *sock, void *data,
                                       size_t len, nb_err *err);

NB_EXPORT nb_status nb_socket_shutdown(nb_socket *sock, bool read_side,
                                       bool write_side, nb_err *err);
NB_EXPORT nb_status nb_socket_local_addr(const nb_socket *sock, nb_addr *out,
                                         nb_err *err);
NB_EXPORT nb_status nb_socket_peer_addr(const nb_socket *sock, nb_addr *out,
                                        nb_err *err);

#ifdef __cplusplus
}
#endif

#endif /* NB_SOCKET_H */
