#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "platform/nb_platform.h"

#include "nb/mem.h"
#include "nb/time.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <sys/resource.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

extern char **environ;

struct nb_process {
    pid_t pid;
    bool new_group;
    bool running;
    bool exited;
    int exit_code;
    int exit_signal;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
};

#define NB_PROCESS_CHUNK 16384u

static bool set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

/* dup2 without leaving FD_CLOEXEC on the target when from == to. */
static bool child_dup(int from, int to)
{
    if (from == to) {
        int flags = fcntl(to, F_GETFD);
        return flags >= 0 && fcntl(to, F_SETFD, flags & ~FD_CLOEXEC) == 0;
    }
    return dup2(from, to) >= 0;
}

/* Only async-signal-safe calls belong between fork and exec. */
static void child_fail(const char *message, size_t len)
{
    const char *text = message;
    while (len > 0) {
        ssize_t n = write(STDERR_FILENO, text, len);
        if (n <= 0) {
            break;
        }
        text += (size_t)n;
        len -= (size_t)n;
    }
    _exit(127);
}

static bool stream_prepare(int fds[2], nb_process_stream mode, int fd, int target, bool input)
{
    if (mode == NB_PROCESS_PIPE) {
        return nb_platform_pipe_open(fds, NULL) == NB_OK;
    }
    int child_fd;
    if (mode == NB_PROCESS_NULL) {
        child_fd = open("/dev/null", (input ? O_RDONLY : O_WRONLY) | O_CLOEXEC);
    } else if (mode == NB_PROCESS_INHERIT || mode == NB_PROCESS_FD) {
        child_fd = fcntl(mode == NB_PROCESS_FD ? fd : target, F_DUPFD_CLOEXEC, 3);
    } else {
        errno = EINVAL;
        return false;
    }
    fds[input ? 0 : 1] = child_fd;
    return child_fd >= 0;
}

static void reset_child_signals(void)
{
    sigset_t empty;
    sigemptyset(&empty);
    (void)sigprocmask(SIG_SETMASK, &empty, NULL);
    const int signals[] = {SIGINT,  SIGQUIT,  SIGTERM, SIGPIPE, SIGHUP,
                           SIGCHLD, SIGWINCH, SIGTSTP, SIGTTIN, SIGTTOU};
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        (void)sigaction(signals[i], &action, NULL);
    }
}

static void close_inherited_descriptors(void)
{
#if defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, 3u, ~0u, 0u) == 0)
        return;
#endif
    struct rlimit limit;
    rlim_t max = getrlimit(RLIMIT_NOFILE, &limit) == 0 ? limit.rlim_cur : 1024;
    if (max == RLIM_INFINITY)
        max = 65536;
    for (rlim_t fd = 3; fd < max; fd++)
        (void)close((int)fd);
}

static void child_setup(const nb_process_options *options, int in_pipe[2], int out_pipe[2],
                        int err_pipe[2], int ready[2])
{
    close_fd(&ready[0]);
    reset_child_signals();
    if (!options->pty) {
        if (options->new_session) {
            if (setsid() < 0) {
                _exit(126);
            }
        } else if (options->new_group && setpgid(0, 0) < 0) {
            _exit(126);
        }
    }
    /* The parent only publishes a child once its group/session exists. */
    char started = 1;
    ssize_t written;
    do {
        written = write(ready[1], &started, sizeof(started));
    } while (written < 0 && errno == EINTR);
    close_fd(&ready[1]);
    if (written != sizeof(started)) {
        _exit(126);
    }
    if (options->cwd && chdir(options->cwd) != 0) {
        child_fail("nbase: chdir failed\n", sizeof("nbase: chdir failed\n") - 1u);
    }
    if (!options->pty) {
        if (!child_dup(in_pipe[0], STDIN_FILENO) || !child_dup(out_pipe[1], STDOUT_FILENO) ||
            !child_dup(options->merge_stderr ? out_pipe[1] : err_pipe[1], STDERR_FILENO)) {
            _exit(127);
        }
    }
    int descriptors[] = {in_pipe[0],  in_pipe[1],  out_pipe[0],
                         out_pipe[1], err_pipe[0], err_pipe[1]};
    for (size_t i = 0; i < sizeof(descriptors) / sizeof(descriptors[0]); i++) {
        if (descriptors[i] > STDERR_FILENO) {
            (void)close(descriptors[i]);
        }
    }
    if (options->close_fds)
        close_inherited_descriptors();
    if (options->env) {
        environ = (char **)options->env;
    }
    if (options->prepare && options->prepare(options->prepare_ctx) != NB_OK) {
        _exit(126);
    }
    if (options->search_path) {
        execvp(options->argv[0], (char *const *)options->argv);
    } else {
        execv(options->argv[0], (char *const *)options->argv);
    }
    child_fail("nbase: exec failed\n", sizeof("nbase: exec failed\n") - 1u);
}

nb_status nb_platform_process_spawn(const nb_process_options *options,
                                    nb_process **out, nb_err *err)
{
    if (!options || !out || !options->argv || !options->argv[0]) {
        nb_err_set(err, NB_ERR_INVAL, "invalid spawn options");
        return NB_ERR_INVAL;
    }
    *out = NULL;

    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int ready[2] = {-1, -1};
    if (nb_platform_pipe_open(ready, NULL) != NB_OK ||
        (!options->pty &&
         (!stream_prepare(in_pipe, options->stdin_mode, options->stdin_fd, STDIN_FILENO, true) ||
          !stream_prepare(out_pipe, options->stdout_mode, options->stdout_fd, STDOUT_FILENO,
                          false) ||
          (!options->merge_stderr && !stream_prepare(err_pipe, options->stderr_mode,
                                                     options->stderr_fd, STDERR_FILENO, false))))) {
        int saved = errno;
        close_fd(&ready[0]);
        close_fd(&ready[1]);
        close_fd(&in_pipe[0]);
        close_fd(&in_pipe[1]);
        close_fd(&out_pipe[0]);
        close_fd(&out_pipe[1]);
        close_fd(&err_pipe[0]);
        close_fd(&err_pipe[1]);
        nb_err_set_os(err, NB_ERR_OS, saved, "pipe failed: %s",
                      strerror(saved));
        return NB_ERR_OS;
    }

    nb_process *process = nb_try_alloc(sizeof(*process));
    if (!process) {
        close_fd(&ready[0]);
        close_fd(&ready[1]);
        close_fd(&in_pipe[0]);
        close_fd(&in_pipe[1]);
        close_fd(&out_pipe[0]);
        close_fd(&out_pipe[1]);
        close_fd(&err_pipe[0]);
        close_fd(&err_pipe[1]);
        nb_err_set(err, NB_ERR_NOMEM, "out of memory creating a process");
        return NB_ERR_NOMEM;
    }

    int master = -1;
    pid_t pid = options->pty ? forkpty(&master, NULL, NULL, NULL) : fork();
    if (pid < 0) {
        int saved = errno;
        close_fd(&ready[0]);
        close_fd(&ready[1]);
        close_fd(&in_pipe[0]);
        close_fd(&in_pipe[1]);
        close_fd(&out_pipe[0]);
        close_fd(&out_pipe[1]);
        close_fd(&err_pipe[0]);
        close_fd(&err_pipe[1]);
        nb_free(process, sizeof(*process));
        nb_err_set_os(err, NB_ERR_OS, saved, "fork failed: %s",
                      strerror(saved));
        return NB_ERR_OS;
    }
    if (pid == 0) {
        child_setup(options, in_pipe, out_pipe, err_pipe, ready);
        _exit(127);
    }

    close_fd(&in_pipe[0]);
    close_fd(&out_pipe[1]);
    close_fd(&err_pipe[1]);

    if (options->new_group && !options->new_session && !options->pty) {
        /* The child also calls setpgid; whoever runs first wins the race. */
        (void)setpgid(pid, pid);
    }

    close_fd(&ready[1]);
    char started = 0;
    ssize_t received;
    do {
        received = read(ready[0], &started, sizeof(started));
    } while (received < 0 && errno == EINTR);
    close_fd(&ready[0]);
    if (received != sizeof(started) || started != 1) {
        (void)kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        }
        close_fd(&in_pipe[1]);
        close_fd(&out_pipe[0]);
        close_fd(&err_pipe[0]);
        close_fd(&master);
        nb_free(process, sizeof(*process));
        nb_err_set(err, NB_ERR_IO, "child session setup failed");
        return NB_ERR_IO;
    }
    if (options->pty) {
        (void)set_cloexec(master);
        out_pipe[0] = master;
        in_pipe[1] = fcntl(master, F_DUPFD_CLOEXEC, 3);
        if (in_pipe[1] < 0) {
            int saved = errno;
            (void)kill(-pid, SIGKILL);
            while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
            }
            close_fd(&master);
            nb_free(process, sizeof(*process));
            return nb_status_from_errno(saved, err);
        }
    }
    process->pid = pid;
    process->new_group = options->new_group || options->new_session || options->pty;
    process->running = true;
    process->exited = false;
    process->exit_code = 0;
    process->exit_signal = 0;
    process->stdin_fd = in_pipe[1];
    process->stdout_fd = out_pipe[0];
    process->stderr_fd = err_pipe[0];
    *out = process;
    return NB_OK;
}

static void record_exit(nb_process *process, int status)
{
    process->running = false;
    process->exited = true;
    if (WIFEXITED(status)) {
        process->exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        process->exit_signal = WTERMSIG(status);
        process->exit_code = 128 + WTERMSIG(status);
    } else {
        process->exit_code = 0;
    }
}

nb_status nb_platform_process_wait(nb_process *process, int timeout_ms,
                                   int *exit_code, bool *exited, nb_err *err)
{
    if (!process) {
        nb_err_set(err, NB_ERR_INVAL, "invalid process");
        return NB_ERR_INVAL;
    }
    if (exit_code) {
        *exit_code = process->exited ? process->exit_code : 0;
    }
    if (exited) {
        *exited = process->exited;
    }
    if (!process->running) {
        return NB_OK;
    }

    bool poll_once = timeout_ms == 0;
    int64_t deadline = -1;
    if (timeout_ms > 0) {
        deadline = nb_platform_time_monotonic_ns() +
                   (int64_t)timeout_ms * INT64_C(1000000);
    }

    for (;;) {
        int status = 0;
        pid_t rc;
        do {
            rc = waitpid(process->pid, &status, WNOHANG);
        } while (rc < 0 && errno == EINTR);

        if (rc == process->pid) {
            record_exit(process, status);
            if (exit_code) {
                *exit_code = process->exit_code;
            }
            if (exited) {
                *exited = true;
            }
            return NB_OK;
        }
        if (rc < 0) {
            int saved = errno;
            nb_err_set_os(err, NB_ERR_OS, saved, "waitpid failed: %s",
                          strerror(saved));
            return NB_ERR_OS;
        }

        if (poll_once) {
            nb_err_set(err, NB_ERR_TIMEOUT, "process is still running");
            return NB_ERR_TIMEOUT;
        }
        if (deadline >= 0 &&
            nb_platform_time_monotonic_ns() >= deadline) {
            nb_err_set(err, NB_ERR_TIMEOUT,
                       "process did not exit within %d ms", timeout_ms);
            return NB_ERR_TIMEOUT;
        }
        nb_platform_sleep_ms(1);
    }
}

static int signal_target(const nb_process *process)
{
    return process->new_group ? -process->pid : process->pid;
}

nb_status nb_platform_process_signal(nb_process *process, int signal,
                                     nb_err *err)
{
    if (!process) {
        nb_err_set(err, NB_ERR_INVAL, "invalid process");
        return NB_ERR_INVAL;
    }
    if (!process->running && !process->new_group) {
        nb_err_set(err, NB_ERR_STATE, "process already exited");
        return NB_ERR_STATE;
    }
    if (signal <= 0) {
        nb_err_set(err, NB_ERR_INVAL, "invalid signal %d", signal);
        return NB_ERR_INVAL;
    }
    if (kill(signal_target(process), signal) != 0) {
        int saved = errno;
        if (saved == ESRCH) {
            nb_err_set_os(err, NB_ERR_STATE, saved,
                          "process no longer exists");
            return NB_ERR_STATE;
        }
        nb_err_set_os(err, NB_ERR_OS, saved, "kill(%d) failed: %s", signal,
                      strerror(saved));
        return NB_ERR_OS;
    }
    return NB_OK;
}

nb_status nb_platform_process_terminate(nb_process *process, int grace_ms)
{
    if (!process) {
        return NB_ERR_INVAL;
    }
    if (!process->running) {
        return NB_OK;
    }

    /* A failed SIGTERM (for example ESRCH) still falls through to SIGKILL and
     * reaping so the handle cannot leak. */
    (void)kill(signal_target(process), SIGTERM);

    int grace = grace_ms < 0 ? 0 : grace_ms;
    int code = 0;
    bool exited = false;
    nb_status status = nb_platform_process_wait(process, grace, &code,
                                                &exited, NULL);
    if (status == NB_OK) {
        return NB_OK;
    }
    if (status != NB_ERR_TIMEOUT) {
        return status;
    }

    (void)kill(signal_target(process), SIGKILL);
    return nb_platform_process_wait(process, -1, NULL, NULL, NULL);
}

static void restore_sigmask(const sigset_t *old)
{
    (void)pthread_sigmask(SIG_SETMASK, old, NULL);
}

static nb_status write_some(int fd, const void *data, size_t len, size_t *written, nb_err *err)
{
    *written = 0;
    sigset_t set, previous, pending;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    int rc = pthread_sigmask(SIG_BLOCK, &set, &previous);
    if (rc != 0) {
        return nb_status_from_errno(rc, err);
    }
    sigemptyset(&pending);
    (void)sigpending(&pending);
    bool already = sigismember(&pending, SIGPIPE) == 1;
    ssize_t count;
    do {
        count = write(fd, data, len);
    } while (count < 0 && errno == EINTR);
    int saved = errno;
    if (count < 0 && saved == EPIPE && !already) {
        (void)sigpending(&pending);
        if (sigismember(&pending, SIGPIPE) == 1) {
            int number = 0;
            (void)sigwait(&set, &number);
        }
    }
    restore_sigmask(&previous);
    if (count < 0) {
        return nb_status_from_errno(saved, err);
    }
    *written = (size_t)count;
    return NB_OK;
}

static nb_status write_all(int fd, const void *data, size_t len, nb_err *err)
{
    size_t offset = 0;
    while (offset < len) {
        size_t written = 0;
        nb_status status = write_some(fd, (const char *)data + offset, len - offset, &written, err);
        if (status != NB_OK)
            return status;
        if (!written)
            return NB_ERR_BROKEN_PIPE;
        offset += written;
    }
    return NB_OK;
}

nb_status nb_platform_process_stdin_write(nb_process *process,
                                          const void *data, size_t len,
                                          nb_err *err)
{
    if (!process || (!data && len != 0)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid process or data");
        return NB_ERR_INVAL;
    }
    if (process->stdin_fd < 0) {
        nb_err_set(err, NB_ERR_STATE, "stdin is closed");
        return NB_ERR_STATE;
    }
    return write_all(process->stdin_fd, data, len, err);
}

nb_status nb_platform_process_close_stdin(nb_process *process, nb_err *err)
{
    if (!process) {
        nb_err_set(err, NB_ERR_INVAL, "invalid process");
        return NB_ERR_INVAL;
    }
    if (process->stdin_fd < 0) {
        return NB_OK;
    }

    int fd = process->stdin_fd;
    process->stdin_fd = -1;
    /* A retried close() after EINTR could close an unrelated reused fd. */
    if (close(fd) != 0 && errno != EINTR) {
        int saved = errno;
        nb_err_set_os(err, NB_ERR_IO, saved, "close stdin failed: %s",
                      strerror(saved));
        return NB_ERR_IO;
    }
    return NB_OK;
}

static int remaining_wait_ms(int64_t deadline, int timeout_ms)
{
    if (timeout_ms < 0) {
        return -1;
    }
    int64_t now = nb_platform_time_monotonic_ns();
    if (deadline < 0 || now >= deadline) {
        return 0;
    }
    int64_t ms = (deadline - now) / INT64_C(1000000);
    if (ms < 1) {
        ms = 1;
    }
    return ms > INT32_MAX ? INT32_MAX : (int)ms;
}

nb_status nb_platform_process_read(nb_process *process, bool stderr_stream,
                                   nb_buf *out, size_t max_bytes,
                                   int timeout_ms, size_t *read_out,
                                   nb_err *err)
{
    if (read_out) {
        *read_out = 0;
    }
    if (!process || !out || !read_out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid process, buffer or output");
        return NB_ERR_INVAL;
    }
    int fd = stderr_stream ? process->stderr_fd : process->stdout_fd;
    if (fd < 0) {
        nb_err_set(err, NB_ERR_INVAL,
                   stderr_stream ? "stderr is merged into stdout"
                                 : "stdout is closed");
        return NB_ERR_INVAL;
    }
    if (max_bytes == 0) {
        return NB_OK;
    }

    int64_t deadline = -1;
    if (timeout_ms >= 0) {
        deadline = nb_platform_time_monotonic_ns() +
                   (int64_t)timeout_ms * INT64_C(1000000);
    }

    unsigned char chunk[NB_PROCESS_CHUNK];
    size_t total = 0;
    bool first = true;
    for (;;) {
        nb_poll_item item;
        memset(&item, 0, sizeof(item));
        item.fd = fd;
        item.want_read = true;

        int wait = first ? remaining_wait_ms(deadline, timeout_ms) : 0;
        first = false;
        size_t ready = 0;
        nb_status status = nb_platform_poll(&item, 1, wait, &ready, err);
        if (status == NB_ERR_TIMEOUT) {
            if (total > 0) {
                nb_err_clear(err);
                break;
            }
            nb_err_set(err, NB_ERR_TIMEOUT, "no output within %d ms",
                       timeout_ms);
            return NB_ERR_TIMEOUT;
        }
        if (status != NB_OK) {
            return status;
        }

        size_t want = NB_MIN(max_bytes - total, sizeof(chunk));
        size_t got = 0;
        status = nb_platform_fd_read(fd, chunk, want, &got, err);
        if (status == NB_ERR_AGAIN) {
            if (total > 0) {
                nb_err_clear(err);
                break;
            }
            nb_err_set(err, NB_ERR_TIMEOUT, "no output within %d ms",
                       timeout_ms);
            return NB_ERR_TIMEOUT;
        }
        if (status != NB_OK) {
            return status;
        }
        if (got == 0) {
            break;
        }
        if (!nb_buf_append(out, chunk, got)) {
            nb_err_set(err, NB_ERR_NOMEM, "out of memory buffering output");
            return NB_ERR_NOMEM;
        }
        total += got;
        if (total >= max_bytes) {
            break;
        }
    }

    *read_out = total;
    return NB_OK;
}

int nb_platform_process_stdin_fd(const nb_process *process)
{
    return process ? process->stdin_fd : -1;
}

int nb_platform_process_stdout_fd(const nb_process *process)
{
    return process ? process->stdout_fd : -1;
}

int nb_platform_process_stderr_fd(const nb_process *process)
{
    return process ? process->stderr_fd : -1;
}

bool nb_platform_process_running(const nb_process *process)
{
    return process ? process->running : false;
}

void nb_platform_process_free(nb_process *process)
{
    if (!process) {
        return;
    }
    if (process->running) {
        (void)kill(signal_target(process), SIGKILL);
        int status = 0;
        pid_t rc;
        do {
            rc = waitpid(process->pid, &status, 0);
        } while (rc < 0 && errno == EINTR);
        process->running = false;
        process->exited = true;
    }
    close_fd(&process->stdin_fd);
    close_fd(&process->stdout_fd);
    close_fd(&process->stderr_fd);
    nb_free(process, sizeof(*process));
}

int64_t nb_platform_process_id(const nb_process *process)
{
    return process ? (int64_t)process->pid : 0;
}
int nb_platform_process_exit_signal(const nb_process *process)
{
    return process ? process->exit_signal : 0;
}
int nb_platform_process_take_stdin(nb_process *process)
{
    int fd = process ? process->stdin_fd : -1;
    if (process)
        process->stdin_fd = -1;
    return fd;
}
int nb_platform_process_take_stdout(nb_process *process)
{
    int fd = process ? process->stdout_fd : -1;
    if (process)
        process->stdout_fd = -1;
    return fd;
}
int nb_platform_process_take_stderr(nb_process *process)
{
    int fd = process ? process->stderr_fd : -1;
    if (process)
        process->stderr_fd = -1;
    return fd;
}

static nb_status exchange_drain(int *fd, nb_buf *out, size_t limit, bool *truncated, nb_err *err)
{
    char bytes[16384];
    ssize_t count = read(*fd, bytes, sizeof(bytes));
    if (count == 0 || (count < 0 && errno == EIO)) {
        close_fd(fd);
        return NB_OK;
    }
    if (count < 0) {
        if (errno == EAGAIN || errno == EINTR)
            return NB_OK;
        return nb_status_from_errno(errno, err);
    }
    size_t keep = nb_buf_len(out) < limit ? limit - nb_buf_len(out) : 0;
    if (keep > (size_t)count)
        keep = (size_t)count;
    if (keep < (size_t)count && truncated)
        *truncated = true;
    return nb_buf_append(out, bytes, keep) ? NB_OK : NB_ERR_NOMEM;
}

nb_status nb_platform_process_exchange(nb_process *process, const void *input, size_t len,
                                       const nb_process_exchange_options *options, nb_buf *output,
                                       nb_buf *error, int *exit_code, nb_err *err)
{
    const nb_process_exchange_options defaults = {.max_output = SIZE_MAX};
    if (!options)
        options = &defaults;
    if (options->stdout_truncated)
        *options->stdout_truncated = false;
    if (options->stderr_truncated)
        *options->stderr_truncated = false;
    int budget = options->timeout_ms > 0 ? options->timeout_ms : 300000;
    int64_t deadline =
        options->timeout_ms < 0 ? INT64_MAX : nb_time_monotonic_ns() + (int64_t)budget * 1000000;
    size_t offset = 0;
    nb_status status = NB_OK;
    int descriptors[] = {process->stdin_fd, process->stdout_fd, process->stderr_fd};
    for (size_t i = 0; i < sizeof(descriptors) / sizeof(descriptors[0]); i++) {
        if (descriptors[i] < 0)
            continue;
        int flags = fcntl(descriptors[i], F_GETFL);
        if (flags < 0 || fcntl(descriptors[i], F_SETFL, flags | O_NONBLOCK) < 0) {
            status = nb_status_from_errno(errno, err);
            break;
        }
    }
    while (status == NB_OK && (process->running || process->stdin_fd >= 0 ||
                               process->stdout_fd >= 0 || process->stderr_fd >= 0)) {
        if (options->cancelled && options->cancelled(options->cancel_ctx)) {
            status = NB_ERR_CANCELLED;
            break;
        }
        if (nb_time_monotonic_ns() >= deadline) {
            status = NB_ERR_TIMEOUT;
            break;
        }
        if (offset == len)
            close_fd(&process->stdin_fd);
        struct pollfd fds[] = {{.fd = process->stdin_fd, .events = POLLOUT},
                               {.fd = process->stdout_fd, .events = POLLIN},
                               {.fd = process->stderr_fd, .events = POLLIN}};
        int ready = poll(fds, 3, 20);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0) {
            status = nb_status_from_errno(errno, err);
            break;
        }
        if (fds[0].fd >= 0 && fds[0].revents) {
            size_t written = 0;
            size_t chunk = len - offset;
            if (chunk > 16384u)
                chunk = 16384u;
            status =
                write_some(process->stdin_fd, (const char *)input + offset, chunk, &written, err);
            if (status == NB_ERR_AGAIN)
                status = NB_OK;
            if (status != NB_OK)
                break;
            offset += written;
        }
        if (fds[1].fd >= 0 && fds[1].revents) {
            status = exchange_drain(&process->stdout_fd, output, options->max_output,
                                    options->stdout_truncated, err);
        }
        if (status == NB_OK && fds[2].fd >= 0 && fds[2].revents) {
            status = exchange_drain(&process->stderr_fd, error, options->max_output,
                                    options->stderr_truncated, err);
        }
        if (status != NB_OK)
            break;
        bool exited = false;
        status = nb_platform_process_wait(process, 0, exit_code, &exited, err);
        if (status == NB_ERR_TIMEOUT)
            status = NB_OK;
        if (exited && offset < len)
            status = NB_ERR_BROKEN_PIPE;
    }
    if (status != NB_OK) {
        /* A reaped leader may still have descendants holding output pipes. */
        (void)nb_platform_process_signal(process, SIGKILL, NULL);
        (void)nb_platform_process_wait(process, -1, exit_code, NULL, NULL);
        nb_err_set(err, status, "%s", nb_status_str(status));
    } else {
        nb_err_clear(err);
    }
    close_fd(&process->stdin_fd);
    return status;
}

nb_status nb_platform_fd_write_until(int fd, const void *data, size_t len, int64_t deadline_ns,
                                     int (*cancelled)(void *), void *ctx, nb_err *err)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return nb_status_from_errno(errno, err);
    }
    size_t offset = 0;
    nb_status status = NB_OK;
    while (offset < len) {
        if (cancelled && cancelled(ctx)) {
            status = NB_ERR_CANCELLED;
            break;
        }
        int64_t remaining = deadline_ns - nb_time_monotonic_ns();
        if (remaining <= 0) {
            status = NB_ERR_TIMEOUT;
            break;
        }
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        int wait = remaining / 1000000 > 20 ? 20 : (int)(remaining / 1000000);
        if (wait < 1)
            wait = 1;
        int ready = poll(&pfd, 1, wait);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0) {
            status = nb_status_from_errno(errno, err);
            break;
        }
        if (!ready)
            continue;
        size_t written = 0;
        status = write_some(fd, (const char *)data + offset, len - offset, &written, err);
        if (status == NB_ERR_AGAIN) {
            status = NB_OK;
            continue;
        }
        if (status != NB_OK)
            break;
        offset += written;
    }
    if (fcntl(fd, F_SETFL, flags) < 0 && status == NB_OK) {
        status = nb_status_from_errno(errno, err);
    }
    return status;
}

nb_status nb_platform_process_stdin_write_until(nb_process *process, const void *data, size_t len,
                                                int64_t deadline_ns, int (*cancelled)(void *),
                                                void *ctx, nb_err *err)
{
    if (process->stdin_fd < 0)
        return NB_ERR_STATE;
    return nb_platform_fd_write_until(process->stdin_fd, data, len, deadline_ns, cancelled, ctx,
                                      err);
}
