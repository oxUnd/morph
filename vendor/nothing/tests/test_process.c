#include "test.h"

#include "nb/buf.h"
#include "nb/poll.h"
#include "nb/process.h"
#include "nb/random.h"
#include "nb/time.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>
#endif

static bool buffer_contains(const nb_buf *buf, const char *needle)
{
    size_t needle_len = strlen(needle);
    size_t len = nb_buf_len(buf);
    const char *data = nb_buf_cstr(buf);
    if (needle_len == 0) {
        return true;
    }
    if (len < needle_len) {
        return false;
    }
    for (size_t i = 0; i + needle_len <= len; i++) {
        if (memcmp(data + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool read_until(nb_process *process, nb_buf *out, bool stderr_stream,
                       const char *needle, int timeout_ms)
{
    int64_t deadline =
        nb_time_monotonic_ns() + (int64_t)timeout_ms * INT64_C(1000000);
    while (!buffer_contains(out, needle)) {
        int64_t now = nb_time_monotonic_ns();
        if (now >= deadline) {
            return false;
        }
        int remaining = (int)((deadline - now) / INT64_C(1000000));
        if (remaining < 1) {
            remaining = 1;
        }
        size_t got = 0;
        nb_err err;
        nb_err_clear(&err);
        nb_status status = nb_process_read(process, stderr_stream, out, 4096,
                                           remaining, &got, &err);
        if (status == NB_ERR_TIMEOUT) {
            return buffer_contains(out, needle);
        }
        if (status != NB_OK) {
            fprintf(stderr, "process read failed: %s\n", err.msg);
            return false;
        }
        if (got == 0) {
            return buffer_contains(out, needle);
        }
    }
    return true;
}

static nb_process *spawn_script_ex(const char *script, bool merge_stderr,
                                   const char *cwd, bool new_group)
{
#ifdef _WIN32
    const char *shell = getenv("COMSPEC");
    if (!shell || shell[0] == '\0') {
        shell = "C:\\Windows\\System32\\cmd.exe";
    }
    const char *argv[] = {shell, "/V:ON", "/C", script, NULL};
#else
    const char *argv[] = {"/bin/sh", "-c", script, NULL};
#endif
    nb_process_options options;
    memset(&options, 0, sizeof(options));
    options.argv = argv;
    options.cwd = cwd;
    options.merge_stderr = merge_stderr;
    options.new_group = new_group;

    nb_err err;
    nb_err_clear(&err);
    nb_process *process = NULL;
    nb_status status = nb_process_spawn(&options, &process, &err);
    if (status != NB_OK) {
        fprintf(stderr, "spawn failed: %s\n", err.msg);
        return NULL;
    }
    return process;
}

static nb_process *spawn_script(const char *script, bool merge_stderr,
                                const char *cwd)
{
    return spawn_script_ex(script, merge_stderr, cwd, false);
}

static void wait_ok(nb_process *process, int expected)
{
    int code = -1;
    bool exited = false;
    nb_err err;
    nb_err_clear(&err);
    NB_CHECK_INT(nb_process_wait(process, 5000, &code, &exited, &err), NB_OK);
    NB_CHECK(exited);
    NB_CHECK_INT(code, expected);
}

static void test_spawn_echo(void)
{
#ifdef _WIN32
    const char *script = "echo hello-world";
#else
    const char *script = "printf 'hello-world'";
#endif
    nb_process *process = spawn_script(script, false, NULL);
    NB_CHECK(process != NULL);
    if (!process) {
        return;
    }

    nb_buf out;
    nb_buf_init(&out);
    NB_CHECK(read_until(process, &out, false, "hello-world", 5000));
    NB_CHECK(nb_process_stdout_fd(process) >= 0);
    NB_CHECK(nb_process_stdin_fd(process) >= 0);
    wait_ok(process, 0);
    NB_CHECK(!nb_process_running(process));
    nb_buf_destroy(&out);
    nb_process_free(process);
}

static void test_exit_code(void)
{
    nb_process *process = spawn_script("exit 7", false, NULL);
    NB_CHECK(process != NULL);
    if (!process) {
        return;
    }
    wait_ok(process, 7);
    nb_process_free(process);
}

static void test_timeout(void)
{
#ifdef _WIN32
    /* Blocks reading stdin until a line arrives or the pipe is closed. */
    const char *script = "set /P x=";
#else
    /* cat inherits stdin, which stays open and idle for this test. */
    const char *script = "cat";
#endif
    nb_process *process = spawn_script_ex(script, false, NULL, true);
    NB_CHECK(process != NULL);
    if (!process) {
        return;
    }

    nb_buf out;
    nb_buf_init(&out);
    size_t got = 0;
    nb_err err;
    nb_err_clear(&err);
    NB_CHECK_INT(nb_process_read(process, false, &out, 16, 50, &got, &err),
                 NB_ERR_TIMEOUT);
    NB_CHECK_INT(got, 0);

    int code = 0;
    bool exited = true;
    NB_CHECK_INT(nb_process_wait(process, 0, &code, &exited, &err),
                 NB_ERR_TIMEOUT);
    NB_CHECK(!exited);
    NB_CHECK(nb_process_running(process));
    NB_CHECK_INT(nb_process_terminate(process, 2000), NB_OK);
    NB_CHECK(!nb_process_running(process));
    nb_buf_destroy(&out);
    nb_process_free(process);
}

static void test_close_stdin(void)
{
#ifdef _WIN32
    /* Read one line and echo it with delayed expansion. */
    const char *script = "set /P x= & echo !x!";
#else
    const char *script = "cat";
#endif
    nb_process *process = spawn_script(script, false, NULL);
    NB_CHECK(process != NULL);
    if (!process) {
        return;
    }

    nb_err err;
    nb_err_clear(&err);
    NB_CHECK_INT(nb_process_stdin_write(process, "hello\n", 6, &err), NB_OK);
    NB_CHECK_INT(nb_process_close_stdin(process, &err), NB_OK);
    NB_CHECK_INT(nb_process_stdin_fd(process), -1);
    /* Closing twice is a successful no-op. */
    NB_CHECK_INT(nb_process_close_stdin(process, &err), NB_OK);
    NB_CHECK_INT(nb_process_stdin_write(process, "x", 1, &err), NB_ERR_STATE);

    nb_buf out;
    nb_buf_init(&out);
    NB_CHECK(read_until(process, &out, false, "hello", 5000));
    /* EOF after the child drains stdin and exits. */
    size_t got = 1;
    int64_t deadline =
        nb_time_monotonic_ns() + (int64_t)5000 * INT64_C(1000000);
    while (got != 0 && nb_time_monotonic_ns() < deadline) {
        nb_err_clear(&err);
        NB_CHECK_INT(nb_process_read(process, false, &out, 4096, 500, &got,
                                     &err),
                     NB_OK);
        if (got == 0) {
            break;
        }
    }
    NB_CHECK_INT(got, 0);
    wait_ok(process, 0);
    nb_buf_destroy(&out);
    nb_process_free(process);
}

#ifndef _WIN32
static void test_stdin_echo(void)
{
    nb_process *process = spawn_script("cat", false, NULL);
    NB_CHECK(process != NULL);
    if (!process) {
        return;
    }
    nb_err err;
    nb_err_clear(&err);
    NB_CHECK_INT(nb_process_stdin_write(process, "ping\n", 5, &err), NB_OK);

    nb_buf out;
    nb_buf_init(&out);
    NB_CHECK(read_until(process, &out, false, "ping", 5000));
    NB_CHECK_INT(nb_process_terminate(process, 1000), NB_OK);
    nb_buf_destroy(&out);
    nb_process_free(process);
}
#endif

static void test_merged_stderr(void)
{
#ifdef _WIN32
    const char *script = "echo out & echo err 1>&2";
#else
    const char *script = "printf out; printf err >&2";
#endif
    nb_process *process = spawn_script(script, true, NULL);
    NB_CHECK(process != NULL);
    if (!process) {
        return;
    }
    NB_CHECK_INT(nb_process_stderr_fd(process), -1);

    nb_buf out;
    nb_buf_init(&out);
    NB_CHECK(read_until(process, &out, false, "out", 5000));
    NB_CHECK(read_until(process, &out, false, "err", 5000));
    wait_ok(process, 0);
    nb_buf_destroy(&out);
    nb_process_free(process);
}

static void test_stderr_stream(void)
{
#ifdef _WIN32
    const char *script = "echo err 1>&2";
#else
    const char *script = "printf err >&2";
#endif
    nb_process *process = spawn_script(script, false, NULL);
    NB_CHECK(process != NULL);
    if (!process) {
        return;
    }
    NB_CHECK(nb_process_stderr_fd(process) >= 0);

    nb_buf out;
    nb_buf_init(&out);
    NB_CHECK(read_until(process, &out, true, "err", 5000));
    wait_ok(process, 0);
    nb_buf_destroy(&out);
    nb_process_free(process);
}

static void test_random(void)
{
    unsigned char a[64];
    unsigned char b[64];
    nb_err err;
    nb_err_clear(&err);
    NB_CHECK_INT(nb_random_bytes(a, sizeof(a), &err), NB_OK);
    NB_CHECK_INT(nb_random_bytes(b, sizeof(b), &err), NB_OK);
    NB_CHECK(memcmp(a, b, sizeof(a)) != 0);

    bool any = false;
    for (size_t i = 0; i < sizeof(a); i++) {
        any = any || a[i] != 0;
    }
    NB_CHECK(any);

    NB_CHECK_INT(nb_random_bytes(NULL, 0, &err), NB_OK);
    NB_CHECK_INT(nb_random_bytes(NULL, 4, &err), NB_ERR_INVAL);
}

#ifdef _WIN32
static void test_poll(void)
{
    WSADATA data;
    NB_CHECK_INT(WSAStartup(MAKEWORD(2, 2), &data), 0);

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    NB_CHECK(listener != INVALID_SOCKET);
    if (listener == INVALID_SOCKET) {
        WSACleanup();
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    NB_CHECK_INT(bind(listener, (const struct sockaddr *)&addr, sizeof(addr)),
                 0);
    NB_CHECK_INT(listen(listener, 1), 0);

    int addr_len = (int)sizeof(addr);
    NB_CHECK_INT(getsockname(listener, (struct sockaddr *)&addr, &addr_len), 0);

    nb_poll_item item;
    memset(&item, 0, sizeof(item));
    item.fd = (int)(uintptr_t)listener;
    item.want_read = true;

    size_t ready = 0;
    nb_err err;
    nb_err_clear(&err);
    NB_CHECK_INT(nb_poll(&item, 1, 0, &ready, &err), NB_ERR_TIMEOUT);
    NB_CHECK_INT(ready, 0);

    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    NB_CHECK(client != INVALID_SOCKET);
    if (client != INVALID_SOCKET) {
        NB_CHECK_INT(connect(client, (const struct sockaddr *)&addr,
                             sizeof(addr)),
                     0);
        NB_CHECK_INT(nb_poll(&item, 1, 2000, &ready, &err), NB_OK);
        NB_CHECK(item.ready_read);
        closesocket(client);
    }
    closesocket(listener);
    WSACleanup();
}
#else
static void test_poll(void)
{
    int fds[2];
    int rc = pipe(fds);
    NB_CHECK_INT(rc, 0);
    if (rc != 0) {
        return;
    }

    nb_poll_item item;
    memset(&item, 0, sizeof(item));
    item.fd = fds[0];
    item.want_read = true;

    size_t ready = 0;
    nb_err err;
    nb_err_clear(&err);
    NB_CHECK_INT(nb_poll(&item, 1, 0, &ready, &err), NB_ERR_TIMEOUT);
    NB_CHECK_INT(ready, 0);

    NB_CHECK_INT((int)write(fds[1], "x", 1), 1);
    NB_CHECK_INT(nb_poll(&item, 1, 2000, &ready, &err), NB_OK);
    NB_CHECK_INT(ready, 1);
    NB_CHECK(item.ready_read);
    NB_CHECK(!item.error);

    char byte = 0;
    NB_CHECK_INT((int)read(fds[0], &byte, 1), 1);
    NB_CHECK_INT(byte, 'x');
    close(fds[0]);
    close(fds[1]);
}
#endif

static void test_spawn_validation(void)
{
    nb_err err;
    nb_err_clear(&err);
    nb_process *process = NULL;
    NB_CHECK_INT(nb_process_spawn(NULL, &process, &err), NB_ERR_INVAL);
    NB_CHECK(process == NULL);

    nb_process_options options;
    memset(&options, 0, sizeof(options));
    NB_CHECK_INT(nb_process_spawn(&options, &process, &err), NB_ERR_INVAL);
    NB_CHECK(process == NULL);
}

static void test_process_exchange(void)
{
#ifdef _WIN32
    const char *script = "set /p value=&echo !value!&echo err 1>&2";
#else
    const char *script = "read value; printf '%s' \"$value\"; printf err >&2";
#endif
    for (int limited = 0; limited < 2; limited++) {
        nb_process *process = spawn_script_ex(script, false, NULL, true);
        if (!process)
            return;
        nb_buf output = {0}, errors = {0};
        bool truncated = false;
        nb_process_exchange_options options = {
            .timeout_ms = 2000,
            .max_output = limited ? 4u : 4096u,
            .stdout_truncated = &truncated,
        };
        int code = -1;
        const char *input = "native-line\n";
        nb_err err;
        nb_err_clear(&err);
        nb_status status = nb_process_exchange(process, input, strlen(input), &options, &output,
                                               &errors, &code, &err);
        NB_CHECK_INT(status, NB_OK);
        NB_CHECK_INT(code, 0);
        NB_CHECK(truncated == (limited != 0));
        if (limited) {
            NB_CHECK_INT(nb_buf_len(&output), 4);
            NB_CHECK_STR(nb_buf_cstr(&output), "nati");
        } else {
            NB_CHECK(buffer_contains(&output, "native-line"));
            NB_CHECK(buffer_contains(&errors, "err"));
        }
        nb_process_free(process);
        nb_buf_destroy(&output);
        nb_buf_destroy(&errors);
    }
}

#ifndef _WIN32
static int always_cancel(void *ctx)
{
    (void)ctx;
    return 1;
}

static void test_extended_process(void)
{
    const char *argv[] = {"sh", "-c", "printf path-ok; printf stderr-ok >&2", NULL};
    nb_process_options spawn = {.argv = argv, .search_path = true, .new_session = true};
    nb_process_exchange_options options = {.timeout_ms = 2000, .max_output = 4096};
    nb_process *process = NULL;
    nb_buf output = {0}, errors = {0};
    nb_err err;
    nb_err_clear(&err);
    int code = -1;
    NB_CHECK(nb_process_spawn(&spawn, &process, &err) == NB_OK);
    if (!process)
        return;
    NB_CHECK(nb_process_id(process) > 0);
    NB_CHECK(nb_process_exchange(process, "", 0, &options, &output, &errors, &code, &err) == NB_OK);
    NB_CHECK_INT(code, 0);
    NB_CHECK_STR(nb_buf_cstr(&output), "path-ok");
    NB_CHECK_STR(nb_buf_cstr(&errors), "stderr-ok");
    nb_process_free(process);
    nb_buf_clear(&output);
    nb_buf_clear(&errors);

    /* Both output pipes fill before the child reads a large request. */
    const char *duplex[] = {"/bin/sh", "-c",
                            "dd if=/dev/zero bs=65536 count=8 2>/dev/null; "
                            "dd if=/dev/zero bs=65536 count=8 1>&2 2>/dev/null; cat",
                            NULL};
    spawn.argv = duplex;
    bool truncated_out = false, truncated_err = false;
    options.max_output = 1024;
    options.stdout_truncated = &truncated_out;
    options.stderr_truncated = &truncated_err;
    nb_buf input = {0};
    NB_CHECK(nb_buf_reserve(&input, 1024u * 1024u));
    for (size_t i = 0; i < 1024u * 1024u; i++) {
        if (!nb_buf_append_char(&input, 'x'))
            break;
    }
    NB_CHECK(nb_process_spawn(&spawn, &process, &err) == NB_OK);
    if (process) {
        NB_CHECK(nb_process_exchange(process, nb_buf_cstr(&input), nb_buf_len(&input), &options,
                                     &output, &errors, &code, &err) == NB_OK);
        NB_CHECK_INT(code, 0);
        NB_CHECK_INT(nb_buf_len(&output), 1024);
        NB_CHECK_INT(nb_buf_len(&errors), 1024);
        NB_CHECK(truncated_out && truncated_err);
        nb_process_free(process);
    }
    nb_buf_destroy(&input);
    nb_buf_clear(&output);
    nb_buf_clear(&errors);

    const char *sleeping[] = {"/bin/sh", "-c", "sleep 10", NULL};
    spawn.argv = sleeping;
    options.cancelled = always_cancel;
    NB_CHECK(nb_process_spawn(&spawn, &process, &err) == NB_OK);
    if (process) {
        NB_CHECK(nb_process_exchange(process, "", 0, &options, &output, &errors, &code, &err) ==
                 NB_ERR_CANCELLED);
        NB_CHECK(!nb_process_running(process));
        nb_process_free(process);
    }
    options.cancelled = NULL;
    options.timeout_ms = 30;
    NB_CHECK(nb_process_spawn(&spawn, &process, &err) == NB_OK);
    if (process) {
        NB_CHECK(nb_process_exchange(process, "", 0, &options, &output, &errors, &code, &err) ==
                 NB_ERR_TIMEOUT);
        NB_CHECK(!nb_process_running(process));
        nb_process_free(process);
    }
    const char *terminal[] = {"/bin/sh", "-c", "test -t 0 && test -t 1 && printf tty-ok", NULL};
    spawn.argv = terminal;
    spawn.pty = true;
    options.timeout_ms = 2000;
    options.max_output = 4096;
    NB_CHECK(nb_process_spawn(&spawn, &process, &err) == NB_OK);
    if (process) {
        NB_CHECK(nb_process_exchange(process, "", 0, &options, &output, &errors, &code, &err) ==
                 NB_OK);
        NB_CHECK_INT(code, 0);
        NB_CHECK_STR(nb_buf_cstr(&output), "tty-ok");
        nb_process_free(process);
    }
    nb_buf_destroy(&output);
    nb_buf_destroy(&errors);
}
#endif

void test_process(void)
{
    test_random();
    test_poll();
    test_spawn_validation();
    test_spawn_echo();
    test_exit_code();
    test_timeout();
    test_close_stdin();
    test_process_exchange();
#ifndef _WIN32
    test_stdin_echo();
    test_extended_process();
#endif
    test_merged_stderr();
    test_stderr_stream();
}
