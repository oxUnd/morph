#include "test.h"

#include "nb/nbase.h"

#include <string.h>

typedef struct cond_state {
    nb_mutex *mutex;
    nb_cond *cond;
    bool ready;
} cond_state;

static void *signal_thread(void *arg)
{
    cond_state *state = arg;
    nb_time_sleep_ms(50);
    nb_mutex_lock(state->mutex);
    state->ready = true;
    nb_cond_signal(state->cond);
    nb_mutex_unlock(state->mutex);
    return NULL;
}

static void test_cond_wait(void)
{
    cond_state state = { nb_mutex_new(0), nb_cond_new(), false };
    NB_CHECK(state.mutex != NULL);
    NB_CHECK(state.cond != NULL);
    if (!state.mutex || !state.cond) {
        nb_cond_free(state.cond);
        nb_mutex_free(state.mutex);
        return;
    }

    nb_thread *thread = NULL;
    NB_CHECK_INT(nb_thread_create(&thread, signal_thread, &state, NULL), NB_OK);
    if (!thread) {
        nb_cond_free(state.cond);
        nb_mutex_free(state.mutex);
        return;
    }

    nb_mutex_lock(state.mutex);
    while (!state.ready) {
        if (nb_cond_timedwait(state.cond, state.mutex, INT64_C(2000000000)) != NB_OK) {
            break;
        }
    }
    NB_CHECK(state.ready);
    nb_mutex_unlock(state.mutex);

    NB_CHECK_INT(nb_thread_join(thread, NULL), NB_OK);
    nb_thread_free(thread);
    nb_cond_free(state.cond);
    nb_mutex_free(state.mutex);

    nb_mutex *mutex = nb_mutex_new(0);
    nb_cond *cond = nb_cond_new();
    NB_CHECK(mutex != NULL);
    NB_CHECK(cond != NULL);
    if (mutex && cond) {
        nb_mutex_lock(mutex);
        int64_t start = nb_time_monotonic_ns();
        nb_status signalled = nb_cond_timedwait(cond, mutex, INT64_C(50000000));
        int64_t elapsed = nb_time_monotonic_ns() - start;
        nb_mutex_unlock(mutex);
        NB_CHECK_INT(signalled, NB_ERR_TIMEOUT);
        NB_CHECK(elapsed >= INT64_C(30000000));
    }
    nb_cond_free(cond);
    nb_mutex_free(mutex);
    NB_CHECK_INT(nb_cond_timedwait(NULL, NULL, 0), NB_ERR_INVAL);
}

typedef struct log_capture {
    char buf[4096];
    size_t len;
    size_t records;
} log_capture;

static void capture_sink(void *ctx, nb_log_level level,
                         const char *record, size_t len)
{
    (void)level;
    log_capture *capture = ctx;
    capture->records++;
    size_t n = len < sizeof(capture->buf) - 1u ? len : sizeof(capture->buf) - 1u;
    if (n != 0) {
        memcpy(capture->buf, record, n);
    }
    capture->buf[n] = '\0';
    capture->len = n;
}

static void test_log_sink(void)
{
    log_capture capture = { { 0 }, 0, 0 };
    nb_log *log = nb_log_new(NB_LOG_INFO, capture_sink, &capture);
    NB_CHECK(log != NULL);
    if (!log) {
        return;
    }

    nb_log_write(log, NB_LOG_DEBUG, "filtered");
    NB_CHECK_INT(capture.records, 0);

    nb_log_write(log, NB_LOG_WARN, "value=%d", 42);
    NB_CHECK_INT(capture.records, 1);
    NB_CHECK(strstr(capture.buf, "[WARN] value=42") != NULL);
    NB_CHECK(capture.len != 0 && capture.buf[capture.len - 1u] == '\n');

    log_capture second = { { 0 }, 0, 0 };
    nb_log_set_sink(log, capture_sink, &second);
    nb_log_write(log, NB_LOG_ERROR, "second sink");
    NB_CHECK_INT(capture.records, 1);
    NB_CHECK_INT(second.records, 1);
    NB_CHECK(strstr(second.buf, "[ERROR] second sink") != NULL);

    static char big[3000];
    memset(big, 'x', sizeof(big));
    nb_log_set_sink(log, capture_sink, &capture);
    nb_log_write(log, NB_LOG_ERROR, "%.*s", (int)sizeof(big), big);
    NB_CHECK_INT(capture.records, 2);
    NB_CHECK(capture.len != 0 && capture.buf[capture.len - 1u] == '\n');
    NB_CHECK(strstr(capture.buf, "[truncated]") == NULL);
    NB_CHECK(capture.len > sizeof(big));
    NB_CHECK(memcmp(capture.buf + capture.len - sizeof(big) - 1u, big, sizeof(big)) == 0);

    nb_log_set_sink(log, NULL, NULL);
    nb_log_free(log);
}

static void *probe_default_log(void *arg)
{
    (void)arg;
    return nb_log_default();
}

static void test_default_log_concurrent(void)
{
    nb_log *expected = nb_log_default();
    NB_CHECK(expected != NULL);

    enum { PROBES = 8 };
    nb_thread *threads[PROBES] = { NULL };
    for (size_t i = 0; i < PROBES; i++) {
        NB_CHECK_INT(nb_thread_create(&threads[i], probe_default_log, NULL,
                                      false),
                     NB_OK);
    }
    for (size_t i = 0; i < PROBES; i++) {
        void *result = NULL;
        if (threads[i]) {
            NB_CHECK_INT(nb_thread_join(threads[i], &result), NB_OK);
            NB_CHECK(result == expected);
            nb_thread_free(threads[i]);
        }
    }

    nb_log *custom = nb_log_new(NB_LOG_OFF, NULL, NULL);
    NB_CHECK(custom != NULL);
    if (custom) {
        nb_log *previous = nb_log_set_default(custom);
        NB_CHECK(nb_log_default() == custom);
        NB_CHECK(nb_log_set_default(previous) == custom);
        nb_log_free(custom);
    }
}

static void test_socket_options(void)
{
    nb_err err;
    nb_err_clear(&err);

    nb_socket *server = NULL;
    NB_CHECK_INT(nb_socket_open(&server, NB_AF_INET, NB_SOCK_STREAM, &err),
                 NB_OK);
    if (!server) {
        return;
    }
    NB_CHECK_INT(nb_socket_set_reuseaddr(server, true, &err), NB_OK);

    nb_addr bind_addr;
    NB_CHECK_INT(nb_addr_parse("127.0.0.1", 0, &bind_addr, &err), NB_OK);
    NB_CHECK_INT(nb_socket_bind(server, &bind_addr, &err), NB_OK);
    NB_CHECK_INT(nb_socket_listen(server, 4, &err), NB_OK);

    nb_addr bound;
    NB_CHECK_INT(nb_socket_local_addr(server, &bound, &err), NB_OK);

    nb_socket *client = NULL;
    NB_CHECK_INT(nb_socket_open(&client, NB_AF_INET, NB_SOCK_STREAM, &err),
                 NB_OK);
    if (!client) {
        nb_socket_free(server);
        return;
    }
    NB_CHECK_INT(nb_socket_connect(client, &bound, 2000, &err), NB_OK);

    nb_socket *peer = NULL;
    NB_CHECK_INT(nb_socket_accept(server, &peer, NULL, &err), NB_OK);
    if (!peer) {
        nb_socket_free(client);
        nb_socket_free(server);
        return;
    }

    NB_CHECK_INT(nb_socket_set_keepalive(client, true, &err), NB_OK);
    NB_CHECK_INT(nb_socket_set_recv_timeout(client, 100, &err), NB_OK);
    NB_CHECK_INT(nb_socket_set_send_timeout(client, 100, &err), NB_OK);
    NB_CHECK_INT(nb_socket_set_keepalive(peer, true, &err), NB_OK);
    NB_CHECK_INT(nb_socket_set_recv_timeout(peer, 0, &err), NB_OK);
    NB_CHECK_INT(nb_socket_set_send_timeout(peer, 0, &err), NB_OK);
    NB_CHECK_INT(nb_socket_set_keepalive(NULL, true, &err), NB_ERR_INVAL);
    NB_CHECK_INT(nb_socket_set_recv_timeout(NULL, 10, &err), NB_ERR_INVAL);
    NB_CHECK_INT(nb_socket_set_send_timeout(NULL, 10, &err), NB_ERR_INVAL);

    char ping[4];
    char pong[4];
    NB_CHECK_INT(nb_socket_send_all(client, "ping", 4, &err), NB_OK);
    NB_CHECK_INT(nb_socket_recv_all(peer, ping, 4, &err), NB_OK);
    NB_CHECK_MEM(ping, "ping", 4);
    NB_CHECK_INT(nb_socket_send_all(peer, "pong", 4, &err), NB_OK);
    NB_CHECK_INT(nb_socket_recv_all(client, pong, 4, &err), NB_OK);
    NB_CHECK_MEM(pong, "pong", 4);
    NB_CHECK_INT(nb_socket_shutdown(client, true, true, &err), NB_OK);

    nb_socket_free(peer);
    nb_socket_free(client);
    nb_socket_free(server);
}

void test_runtime(void)
{
    test_cond_wait();
    test_log_sink();
    test_default_log_concurrent();
    test_socket_options();
}
