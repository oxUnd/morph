#include "nb/nbase.h"

#include <stdio.h>

typedef struct server_state {
    nb_socket *server;
    nb_status status;
} server_state;

static void *echo_server(void *arg)
{
    server_state *state = arg;
    nb_err err;
    nb_err_clear(&err);
    nb_socket *client = NULL;

    for (int attempt = 0; attempt < 500; attempt++) {
        state->status = nb_socket_accept(state->server, &client, NULL, &err);
        if (state->status == NB_OK) {
            break;
        }
        if (state->status != NB_ERR_AGAIN) {
            return NULL;
        }
        nb_time_sleep_ms(10);
    }
    if (state->status != NB_OK) {
        return NULL;
    }

    nb_socket_set_nonblock(client, false, &err);
    char request[6] = { 0 };
    state->status = nb_socket_recv_all(client, request, 5, &err);
    if (state->status == NB_OK) {
        state->status = nb_socket_send_all(client, "world", 5, &err);
    }
    nb_socket_free(client);
    return NULL;
}

int main(void)
{
    nb_err err;
    nb_err_clear(&err);
    nb_socket *server = NULL;
    if (nb_socket_open(&server, NB_AF_INET, NB_SOCK_STREAM, &err) != NB_OK) {
        printf("open failed: %s\n", err.msg);
        return 1;
    }
    nb_socket_set_reuseaddr(server, true, &err);
    nb_socket_set_nonblock(server, true, &err);

    nb_addr bind_addr;
    nb_addr_parse("127.0.0.1", 0, &bind_addr, &err);
    if (nb_socket_bind(server, &bind_addr, &err) != NB_OK ||
        nb_socket_listen(server, 8, &err) != NB_OK) {
        printf("listen failed: %s\n", err.msg);
        nb_socket_free(server);
        return 1;
    }
    nb_addr local;
    nb_socket_local_addr(server, &local, &err);
    printf("listening on 127.0.0.1:%u\n", (unsigned)local.port);

    server_state state = { server, NB_ERR_STATE };
    nb_thread *thread = NULL;
    if (nb_thread_create(&thread, echo_server, &state, NULL) != NB_OK) {
        nb_socket_free(server);
        return 1;
    }

    nb_socket *client = NULL;
    nb_socket_open(&client, NB_AF_INET, NB_SOCK_STREAM, &err);
    if (!client ||
        nb_socket_connect(client, &local, 2000, &err) != NB_OK) {
        printf("client failed: %s\n", err.msg);
    } else if (nb_socket_set_keepalive(client, true, &err) != NB_OK ||
               nb_socket_set_recv_timeout(client, 2000, &err) != NB_OK ||
               nb_socket_set_send_timeout(client, 2000, &err) != NB_OK ||
               nb_socket_send_all(client, "hello", 5, &err) != NB_OK) {
        printf("client setup failed: %s\n", err.msg);
    } else {
        char reply[6] = { 0 };
        nb_socket_recv_all(client, reply, 5, &err);
        printf("client received: %s\n", reply);
    }

    nb_socket_free(client);
    nb_thread_join(thread, NULL);
    nb_thread_free(thread);
    printf("server status=%d\n", (int)state.status);
    nb_socket_free(server);
    return 0;
}
