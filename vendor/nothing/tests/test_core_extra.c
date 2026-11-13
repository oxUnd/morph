#include "test.h"

#include "nb/nbase.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

static void test_buf_detach(void)
{
    size_t cap = 0;
    nb_buf *buf = nb_buf_new();
    NB_CHECK(buf != NULL);
    if (!buf) {
        return;
    }

    NB_CHECK(nb_buf_detach(buf, &cap) == NULL);
    NB_CHECK(nb_buf_detach(NULL, &cap) == NULL);

    NB_CHECK(nb_buf_append_cstr(buf, "abcdef"));
    nb_buf_consume(buf, 2);
    char *data = nb_buf_detach(buf, &cap);
    NB_CHECK(data != NULL);
    if (data) {
        NB_CHECK_STR(data, "cdef");
        NB_CHECK_INT(strlen(data), 4);
        NB_CHECK(cap >= 5);
        nb_free(data, cap);
    }
    NB_CHECK(nb_buf_empty(buf));
    NB_CHECK(buf->ptr == NULL);
    NB_CHECK_INT(buf->cap, 0);
    NB_CHECK_INT(buf->off, 0);

    /* A missing capacity output would make the result impossible to free. */
    NB_CHECK(nb_buf_append_cstr(buf, "x"));
    NB_CHECK(nb_buf_detach(buf, NULL) == NULL);
    NB_CHECK_INT(nb_buf_len(buf), 1);

    /* Fully consumed storage counts as empty and must not be detached. */
    nb_buf_consume(buf, 1);
    NB_CHECK(nb_buf_detach(buf, &cap) == NULL);
    NB_CHECK_INT(nb_buf_len(buf), 0);
    nb_buf_free(buf);
}

static void test_buf_read_fd(void)
{
    static const char message[] = "hello fd";
    const size_t message_len = sizeof(message) - 1u;
    int fds[2] = { -1, -1 };

#if defined(_WIN32)
    if (_pipe(fds, 4096, _O_BINARY) != 0) {
        NB_CHECK(false);
        return;
    }
    if (_write(fds[1], message, (unsigned)message_len) !=
        (int)message_len) {
        NB_CHECK(false);
        _close(fds[0]);
        _close(fds[1]);
        return;
    }
    _close(fds[1]);
#else
    if (pipe(fds) != 0) {
        NB_CHECK(false);
        return;
    }
    if (write(fds[1], message, message_len) != (ssize_t)message_len) {
        NB_CHECK(false);
        close(fds[0]);
        close(fds[1]);
        return;
    }
    close(fds[1]);
#endif

    nb_buf *buf = nb_buf_new();
    nb_err err;
    nb_err_clear(&err);
    NB_CHECK(buf != NULL);
    if (!buf) {
#if defined(_WIN32)
        _close(fds[0]);
#else
        close(fds[0]);
#endif
        return;
    }

    size_t read = 123;
    NB_CHECK_INT(nb_buf_read_fd(buf, fds[0], 64, &read, &err), NB_OK);
    NB_CHECK_INT(read, message_len);
    NB_CHECK_INT(nb_buf_len(buf), message_len);
    NB_CHECK_MEM(nb_buf_cstr(buf), message, message_len);

    /* EOF is a successful zero-byte read. */
    read = 123;
    NB_CHECK_INT(nb_buf_read_fd(buf, fds[0], 64, &read, &err), NB_OK);
    NB_CHECK_INT(read, 0);
    NB_CHECK_INT(nb_buf_len(buf), message_len);

    read = 123;
    NB_CHECK_INT(nb_buf_read_fd(buf, fds[0], 0, &read, &err), NB_OK);
    NB_CHECK_INT(read, 0);

    NB_CHECK_INT(nb_buf_read_fd(NULL, fds[0], 1, &read, &err),
                 NB_ERR_INVAL);
    NB_CHECK_INT(nb_buf_read_fd(buf, -1, 1, &read, &err), NB_ERR_INVAL);
    nb_buf_free(buf);

#if defined(_WIN32)
    _close(fds[0]);
#else
    close(fds[0]);
#endif
}

static void test_buf_next_line(void)
{
    nb_buf *buf = nb_buf_new();
    NB_CHECK(buf != NULL);
    if (!buf) {
        return;
    }
    nb_str line;

    NB_CHECK(nb_buf_append_cstr(buf, "one\r\ntwo\nthree"));
    const size_t initial = nb_buf_len(buf);
    NB_CHECK(nb_buf_next_line(buf, &line));
    NB_CHECK_INT(line.len, 3);
    NB_CHECK_MEM(line.ptr, "one", 3);
    NB_CHECK_INT(nb_buf_len(buf), initial - 5u);

    NB_CHECK(nb_buf_next_line(buf, &line));
    NB_CHECK_INT(line.len, 3);
    NB_CHECK_MEM(line.ptr, "two", 3);
    NB_CHECK_INT(nb_buf_len(buf), 5);

    NB_CHECK(!nb_buf_next_line(buf, &line));
    NB_CHECK_INT(nb_buf_len(buf), 5);
    NB_CHECK(nb_buf_append_cstr(buf, "\n"));
    NB_CHECK(nb_buf_next_line(buf, &line));
    NB_CHECK_INT(line.len, 5);
    NB_CHECK_MEM(line.ptr, "three", 5);
    NB_CHECK(nb_buf_empty(buf));
    NB_CHECK(!nb_buf_next_line(buf, &line));

    /* Empty lines still count, and a lone CR is not stripped. */
    nb_buf_clear(buf);
    NB_CHECK(nb_buf_append_cstr(buf, "\r\n\nx\ry"));
    NB_CHECK(nb_buf_next_line(buf, &line));
    NB_CHECK_INT(line.len, 0);
    NB_CHECK(nb_buf_next_line(buf, &line));
    NB_CHECK_INT(line.len, 0);
    NB_CHECK(!nb_buf_next_line(buf, &line));
    NB_CHECK(nb_buf_append_cstr(buf, "\n"));
    NB_CHECK(nb_buf_next_line(buf, &line));
    NB_CHECK_INT(line.len, 3);
    NB_CHECK_MEM(line.ptr, "x\ry", 3);

    NB_CHECK(!nb_buf_next_line(NULL, &line));
    NB_CHECK(!nb_buf_next_line(buf, NULL));
    nb_buf_free(buf);
}

static void test_utf8_width(void)
{
    NB_CHECK_INT(nb_utf8_width_cp('A'), 1);
    NB_CHECK_INT(nb_utf8_width_cp('0'), 1);
    NB_CHECK_INT(nb_utf8_width_cp(0x0000u), 0);
    NB_CHECK_INT(nb_utf8_width_cp(0x001Fu), 0);
    NB_CHECK_INT(nb_utf8_width_cp(0x007Fu), 0);
    NB_CHECK_INT(nb_utf8_width_cp(0x009Fu), 0);
    NB_CHECK_INT(nb_utf8_width_cp(0x00A1u), 1);   /* ambiguous */
    NB_CHECK_INT(nb_utf8_width_cp(0x00E9u), 1);

    /* Combining marks, joiners, variation selectors and format controls. */
    NB_CHECK_INT(nb_utf8_width_cp(0x0301u), 0);
    NB_CHECK_INT(nb_utf8_width_cp(0x0483u), 0);
    NB_CHECK_INT(nb_utf8_width_cp(0x200Bu), 0);
    NB_CHECK_INT(nb_utf8_width_cp(0x200Du), 0);
    NB_CHECK_INT(nb_utf8_width_cp(0x20E3u), 0);
    NB_CHECK_INT(nb_utf8_width_cp(0xFE0Fu), 0);
    NB_CHECK_INT(nb_utf8_width_cp(0xFEFFu), 0);

    /* East Asian wide/fullwidth, Hangul and common emoji. */
    NB_CHECK_INT(nb_utf8_width_cp(0x4E2Du), 2);
    NB_CHECK_INT(nb_utf8_width_cp(0x3000u), 2);
    NB_CHECK_INT(nb_utf8_width_cp(0xFF21u), 2);
    NB_CHECK_INT(nb_utf8_width_cp(0xAC00u), 2);
    NB_CHECK_INT(nb_utf8_width_cp(0x1F600u), 2);
    NB_CHECK_INT(nb_utf8_width_cp(0x1F680u), 2);

    NB_CHECK_INT(nb_utf8_width_cp(0xD800u), -1);
    NB_CHECK_INT(nb_utf8_width_cp(0x110000u), -1);

    NB_CHECK_INT(nb_utf8_width(NB_STR_LIT("A\xE4\xB8\xAD\xF0\x9F\x98\x80")),
                 5);
    NB_CHECK_INT(nb_utf8_width(NB_STR_LIT("e\xCC\x81")), 1);
    NB_CHECK_INT(nb_utf8_width(NB_STR_LIT("A\xFFZ")), 1);

    const char cjk[] = "\xE4\xB8\xAD";
    nb_str s = nb_str_from_parts(cjk, sizeof(cjk) - 1u);
    size_t index = 0;
    int width = -1;
    NB_CHECK(nb_utf8_width_at(s, &index, &width));
    NB_CHECK_INT(index, 3);
    NB_CHECK_INT(width, 2);

    const char combining[] = "e\xCC\x81";
    s = nb_str_from_parts(combining, sizeof(combining) - 1u);
    index = 0;
    NB_CHECK(nb_utf8_width_at(s, &index, &width));
    NB_CHECK_INT(index, 1);
    NB_CHECK_INT(width, 1);
    NB_CHECK(nb_utf8_width_at(s, &index, &width));
    NB_CHECK_INT(index, 3);
    NB_CHECK_INT(width, 0);
    NB_CHECK(!nb_utf8_width_at(s, &index, &width));

    const char invalid[] = "\xFF";
    s = nb_str_from_parts(invalid, sizeof(invalid) - 1u);
    index = 0;
    NB_CHECK(!nb_utf8_width_at(s, &index, &width));
    NB_CHECK_INT(index, 0);
    NB_CHECK(!nb_utf8_width_at(s, NULL, &width));
    NB_CHECK(!nb_utf8_width_at(s, &index, NULL));
}

static void test_recursive_mutex(void)
{
    nb_mutex *recursive = nb_mutex_new(NB_MUTEX_RECURSIVE);
    NB_CHECK(recursive != NULL);
    if (recursive) {
        nb_mutex_lock(recursive);
        nb_mutex_lock(recursive);
        nb_mutex_unlock(recursive);
        nb_mutex_unlock(recursive);
        nb_mutex_free(recursive);
    }

    nb_mutex *plain = nb_mutex_new(0);
    NB_CHECK(plain != NULL);
    if (plain) {
        nb_mutex_lock(plain);
        nb_mutex_unlock(plain);
        nb_mutex_free(plain);
    }
}

typedef struct wait_state {
    nb_mutex *mutex;
    nb_cond *cond;
    bool ready;
} wait_state;

static void *signal_wait_state(void *arg)
{
    wait_state *state = arg;
    nb_time_sleep_ms(30);
    nb_mutex_lock(state->mutex);
    state->ready = true;
    nb_cond_signal(state->cond);
    nb_mutex_unlock(state->mutex);
    return NULL;
}

static void test_cond_wait_until(void)
{
    nb_mutex *mutex = nb_mutex_new(0);
    nb_cond *cond = nb_cond_new();
    NB_CHECK(mutex != NULL);
    NB_CHECK(cond != NULL);
    if (!mutex || !cond) {
        nb_cond_free(cond);
        nb_mutex_free(mutex);
        return;
    }

    nb_mutex_lock(mutex);
    int64_t start = nb_time_monotonic_ns();
    nb_status signalled = nb_cond_wait_until(cond, mutex, start + INT64_C(50000000));
    int64_t elapsed = nb_time_monotonic_ns() - start;
    nb_mutex_unlock(mutex);
    NB_CHECK_INT(signalled, NB_ERR_TIMEOUT);
    NB_CHECK(elapsed >= INT64_C(20000000));

    /* A deadline already in the past times out immediately. */
    nb_mutex_lock(mutex);
    start = nb_time_monotonic_ns();
    signalled = nb_cond_wait_until(cond, mutex, start - 1);
    elapsed = nb_time_monotonic_ns() - start;
    nb_mutex_unlock(mutex);
    NB_CHECK_INT(signalled, NB_ERR_TIMEOUT);
    NB_CHECK(elapsed < INT64_C(1000000000));

    wait_state state = { mutex, cond, false };
    nb_thread *thread = NULL;
    NB_CHECK_INT(nb_thread_create(&thread, signal_wait_state, &state, NULL), NB_OK);
    if (thread) {
        nb_mutex_lock(mutex);
        int64_t deadline = nb_time_monotonic_ns() + INT64_C(2000000000);
        while (!state.ready) {
            if (nb_cond_wait_until(cond, mutex, deadline) != NB_OK) {
                break;
            }
        }
        NB_CHECK(state.ready);
        nb_mutex_unlock(mutex);
        NB_CHECK_INT(nb_thread_join(thread, NULL), NB_OK);
        nb_thread_free(thread);
    }

    nb_cond_free(cond);
    nb_mutex_free(mutex);
    NB_CHECK_INT(nb_cond_wait_until(NULL, NULL, 0), NB_ERR_INVAL);
}

static void *thread_identity_worker(void *arg)
{
    nb_thread_id *seen = arg;
    *seen = nb_thread_current_id();
    return arg;
}

static void test_thread_owners(void)
{
    nb_thread_id owner = nb_thread_current_id();
    NB_CHECK(nb_thread_id_equal(owner, nb_thread_current_id()));
    nb_thread_id seen = {0};
    nb_thread *thread = NULL;
    nb_thread_options options = {false, 1024u * 1024u};
    NB_CHECK_INT(nb_thread_create(&thread, thread_identity_worker, &seen, &options), NB_OK);
    if (!thread) {
        return;
    }
    NB_CHECK(!nb_thread_is_current(thread));
    void *result = NULL;
    NB_CHECK_INT(nb_thread_join_take(&thread, &result), NB_OK);
    NB_CHECK(thread == NULL);
    NB_CHECK(result == &seen);
    NB_CHECK(!nb_thread_id_equal(owner, seen));
    NB_CHECK_INT(nb_thread_join_take(&thread, NULL), NB_ERR_INVAL);
    NB_CHECK_INT(nb_thread_detach_take(NULL), NB_ERR_INVAL);
    nb_mutex *mutex = nb_mutex_new(0);
    nb_cond *cond = nb_cond_new();
    if (mutex && cond) {
        nb_mutex_lock(mutex);
        NB_CHECK_INT(nb_cond_wait_realtime_until(cond, mutex, 0), NB_ERR_TIMEOUT);
        nb_mutex_unlock(mutex);
    }
    nb_cond_free(cond);
    nb_mutex_free(mutex);
}

static void test_monotonic_deadlines(void)
{
    int64_t before = nb_time_monotonic_ns();
    int64_t immediate = nb_time_deadline_after_ns(-1);
    int64_t after = nb_time_monotonic_ns();
    NB_CHECK(immediate >= before && immediate <= after);
    NB_CHECK_INT(nb_time_remaining_ns(0), 0);
    NB_CHECK_INT(nb_time_deadline_after_ns(INT64_MAX), INT64_MAX);
    int64_t future = nb_time_deadline_after_ns(INT64_C(2000000000));
    NB_CHECK(future > after);
    NB_CHECK(nb_time_remaining_ns(future) > 0);
    NB_CHECK(nb_time_remaining_ns(future) <= INT64_C(2000000000));
}

static void test_cleanup_macros(void)
{
    {
        nb_buf guarded NB_CLEANUP(nb_buf_cleanup);
        nb_buf_init(&guarded);
        NB_CHECK(nb_buf_append_cstr(&guarded, "scoped"));
    }
    {
        nb_thread *thread NB_CLEANUP(nb_thread_cleanup) = NULL;
        nb_map *map NB_CLEANUP(nb_map_cleanup) = NULL;
        nb_socket *sock NB_CLEANUP(nb_socket_cleanup) = NULL;
        NB_CHECK(thread == NULL);
        NB_CHECK(map == NULL);
        NB_CHECK(sock == NULL);
    }
}

static void test_cancel(void)
{
    nb_cancel cancel;
    nb_cancel_init(&cancel);
    NB_CHECK(!nb_cancel_requested(&cancel));

    nb_cancel_request(&cancel);
    NB_CHECK(nb_cancel_requested(&cancel));
    nb_cancel_request(&cancel);
    NB_CHECK(nb_cancel_requested(&cancel));

    nb_cancel_reset(&cancel);
    NB_CHECK(!nb_cancel_requested(&cancel));

    nb_cancel_init(&cancel);
    NB_CHECK(!nb_cancel_requested(&cancel));

    nb_cancel_init(NULL);
    nb_cancel_reset(NULL);
    nb_cancel_request(NULL);
    NB_CHECK(!nb_cancel_requested(NULL));
}

void test_core_extra(void)
{
    test_buf_detach();
    test_buf_read_fd();
    test_buf_next_line();
    test_utf8_width();
    test_recursive_mutex();
    test_cond_wait_until();
    test_thread_owners();
    test_monotonic_deadlines();
    test_cleanup_macros();
    test_cancel();
}
