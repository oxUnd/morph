#include "platform/nb_platform.h"

#include "nb/mem.h"
#include "nb/thread.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#ifndef SIGKILL
#define SIGKILL 9
#endif

#define NB_PROCESS_CHUNK 16384u

struct nb_process {
    HANDLE process;
    HANDLE job;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    DWORD exit_code;
    bool running;
    bool exited;
};

static void close_handle(HANDLE *handle)
{
    if (*handle && *handle != INVALID_HANDLE_VALUE) {
        CloseHandle(*handle);
        *handle = NULL;
    }
}

static nb_status set_win_error(nb_err *err, DWORD code, const char *what)
{
    nb_status status =
        code == ERROR_INVALID_PARAMETER ? NB_ERR_INVAL : NB_ERR_OS;
    nb_err_set_os(err, status, (int)code, "%s failed (win32 error %lu)", what,
                  (unsigned long)code);
    return status;
}

static wchar_t *utf8_to_wide(const char *text, nb_err *err)
{
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                                     NULL, 0);
    if (needed <= 0) {
        DWORD code = GetLastError();
        if (code == ERROR_NO_UNICODE_TRANSLATION) {
            nb_err_set_os(err, NB_ERR_INVAL, (int)code,
                          "text is not valid UTF-8");
            return NULL;
        }
        (void)set_win_error(err, code, "MultiByteToWideChar");
        return NULL;
    }
    wchar_t *wide = nb_try_alloc((size_t)needed * sizeof(wchar_t));
    if (!wide) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory converting text");
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide,
                            needed) == 0) {
        DWORD code = GetLastError();
        nb_free(wide, (size_t)needed * sizeof(wchar_t));
        if (code == ERROR_NO_UNICODE_TRANSLATION) {
            nb_err_set_os(err, NB_ERR_INVAL, (int)code,
                          "text is not valid UTF-8");
            return NULL;
        }
        (void)set_win_error(err, code, "MultiByteToWideChar");
        return NULL;
    }
    return wide;
}

/* Builds a NUL-separated, double-NUL-terminated UTF-16 environment block. */
static wchar_t *build_env_block(const char *const *env, size_t *chars_out, nb_err *err)
{
    size_t chars = 0;
    for (size_t i = 0; env[i]; i++) {
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, env[i], -1,
                                    NULL, 0);
        if (n <= 0) {
            DWORD code = GetLastError();
            if (code == ERROR_NO_UNICODE_TRANSLATION) {
                nb_err_set_os(err, NB_ERR_INVAL, (int)code,
                              "environment is not valid UTF-8");
            } else {
                (void)set_win_error(err, code, "MultiByteToWideChar");
            }
            return NULL;
        }
        if (chars > (SIZE_MAX / sizeof(wchar_t)) - (size_t)n - 2u) {
            nb_err_set(err, NB_ERR_NOMEM, "environment block is too large");
            return NULL;
        }
        chars += (size_t)n;
    }

    wchar_t *block = nb_try_alloc((chars + 2u) * sizeof(wchar_t));
    if (!block) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory building environment");
        return NULL;
    }
    wchar_t *cursor = block;
    for (size_t i = 0; env[i]; i++) {
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, env[i], -1,
                                    cursor, INT_MAX);
        if (n <= 0) {
            DWORD code = GetLastError();
            nb_free(block, (chars + 2u) * sizeof(wchar_t));
            (void)set_win_error(err, code, "MultiByteToWideChar");
            return NULL;
        }
        cursor += n;
    }
    cursor[0] = L'\0';
    cursor[1] = L'\0';
    *chars_out = chars + 2u;
    return block;
}

static bool append_quoted_arg(nb_buf *buf, const char *arg)
{
    bool needs_quotes = arg[0] == '\0';
    for (const char *p = arg; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '"') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return nb_buf_append_cstr(buf, arg);
    }

    if (!nb_buf_append_char(buf, '"')) {
        return false;
    }
    const char *p = arg;
    while (*p) {
        size_t backslashes = 0;
        while (*p == '\\') {
            backslashes++;
            p++;
        }
        if (*p == '"') {
            for (size_t i = 0; i < backslashes * 2u + 1u; i++) {
                if (!nb_buf_append_char(buf, '\\')) {
                    return false;
                }
            }
            if (!nb_buf_append_char(buf, '"')) {
                return false;
            }
            p++;
        } else if (*p == '\0') {
            for (size_t i = 0; i < backslashes * 2u; i++) {
                if (!nb_buf_append_char(buf, '\\')) {
                    return false;
                }
            }
        } else {
            for (size_t i = 0; i < backslashes; i++) {
                if (!nb_buf_append_char(buf, '\\')) {
                    return false;
                }
            }
            if (!nb_buf_append_char(buf, *p)) {
                return false;
            }
            p++;
        }
    }
    return nb_buf_append_char(buf, '"');
}

static bool build_command_line(const char *const *argv, nb_buf *out)
{
    for (size_t i = 0; argv[i]; i++) {
        if (i != 0 && !nb_buf_append_char(out, ' ')) {
            return false;
        }
        if (!append_quoted_arg(out, argv[i])) {
            return false;
        }
    }
    return true;
}

typedef struct spawn_state {
    HANDLE in_read;
    HANDLE in_write;
    HANDLE out_read;
    HANDLE out_write;
    HANDLE err_read;
    HANDLE err_write;
    HANDLE job;
    PROCESS_INFORMATION info;
    bool created;
} spawn_state;

static void spawn_state_clear(spawn_state *state)
{
    if (state->created && state->info.hProcess) {
        (void)TerminateProcess(state->info.hProcess, 1);
        (void)WaitForSingleObject(state->info.hProcess, INFINITE);
    }
    close_handle(&state->in_read);
    close_handle(&state->in_write);
    close_handle(&state->out_read);
    close_handle(&state->out_write);
    close_handle(&state->err_read);
    close_handle(&state->err_write);
    close_handle(&state->info.hThread);
    close_handle(&state->info.hProcess);
    close_handle(&state->job);
}

static int adopt_handle(HANDLE *handle, nb_err *err)
{
    int fd = _open_osfhandle((intptr_t)*handle, _O_BINARY);
    if (fd < 0) {
        nb_err_set_os(err, NB_ERR_OS, errno,
                      "_open_osfhandle failed: %s", strerror(errno));
        return -1;
    }
    *handle = NULL;
    return fd;
}

static nb_status create_job(HANDLE *out, nb_err *err)
{
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job) {
        return set_win_error(err, GetLastError(), "CreateJobObjectW");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    memset(&limits, 0, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        DWORD code = GetLastError();
        CloseHandle(job);
        return set_win_error(err, code, "SetInformationJobObject");
    }
    *out = job;
    return NB_OK;
}

static nb_status configure_stream(HANDLE *child, HANDLE *parent, nb_process_stream mode, int fd,
                                  DWORD standard, bool input, nb_err *err)
{
    if (mode == NB_PROCESS_PIPE)
        return NB_OK;
    HANDLE replacement = NULL;
    if (mode == NB_PROCESS_NULL) {
        SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
        replacement = CreateFileW(L"NUL", input ? GENERIC_READ : GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, NULL);
        if (replacement == INVALID_HANDLE_VALUE)
            return set_win_error(err, GetLastError(), "open NUL");
    } else if (mode == NB_PROCESS_INHERIT || mode == NB_PROCESS_FD) {
        HANDLE source =
            mode == NB_PROCESS_INHERIT ? GetStdHandle(standard) : (HANDLE)_get_osfhandle(fd);
        if (!source || source == INVALID_HANDLE_VALUE)
            return nb_status_from_errno(EBADF, err);
        if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &replacement, 0,
                             TRUE, DUPLICATE_SAME_ACCESS))
            return set_win_error(err, GetLastError(), "DuplicateHandle");
    } else {
        return NB_ERR_INVAL;
    }
    close_handle(child);
    close_handle(parent);
    *child = replacement;
    return NB_OK;
}

nb_status nb_platform_process_spawn(const nb_process_options *options,
                                    nb_process **out, nb_err *err)
{
    if (!options || !out || !options->argv || !options->argv[0]) {
        nb_err_set(err, NB_ERR_INVAL, "invalid spawn options");
        return NB_ERR_INVAL;
    }
    *out = NULL;
    if (options->prepare || options->pty || options->new_session) {
        nb_err_set(err, NB_ERR_UNSUPPORTED, "requested process options are not supported");
        return NB_ERR_UNSUPPORTED;
    }

    nb_process *process = nb_try_alloc(sizeof(*process));
    if (!process) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory creating a process");
        return NB_ERR_NOMEM;
    }
    memset(process, 0, sizeof(*process));
    process->stdin_fd = -1;
    process->stdout_fd = -1;
    process->stderr_fd = -1;

    spawn_state state;
    memset(&state, 0, sizeof(state));

    SECURITY_ATTRIBUTES inherit;
    inherit.nLength = sizeof(inherit);
    inherit.lpSecurityDescriptor = NULL;
    inherit.bInheritHandle = TRUE;

    nb_status status = NB_OK;
    if (!CreatePipe(&state.in_read, &state.in_write, &inherit, 0) ||
        !CreatePipe(&state.out_read, &state.out_write, &inherit, 0) ||
        (!options->merge_stderr &&
         !CreatePipe(&state.err_read, &state.err_write, &inherit, 0))) {
        status = set_win_error(err, GetLastError(), "CreatePipe");
        spawn_state_clear(&state);
        nb_free(process, sizeof(*process));
        return status;
    }

    /* Only the child-side ends may cross CreateProcessW. */
    if (!SetHandleInformation(state.in_write, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(state.out_read, HANDLE_FLAG_INHERIT, 0) ||
        (!options->merge_stderr &&
         !SetHandleInformation(state.err_read, HANDLE_FLAG_INHERIT, 0))) {
        status = set_win_error(err, GetLastError(), "SetHandleInformation");
        spawn_state_clear(&state);
        nb_free(process, sizeof(*process));
        return status;
    }

    status = configure_stream(&state.in_read, &state.in_write, options->stdin_mode,
                              options->stdin_fd, STD_INPUT_HANDLE, true, err);
    if (status == NB_OK)
        status = configure_stream(&state.out_write, &state.out_read, options->stdout_mode,
                                  options->stdout_fd, STD_OUTPUT_HANDLE, false, err);
    if (status == NB_OK && !options->merge_stderr)
        status = configure_stream(&state.err_write, &state.err_read, options->stderr_mode,
                                  options->stderr_fd, STD_ERROR_HANDLE, false, err);
    if (status != NB_OK) {
        spawn_state_clear(&state);
        nb_free(process, sizeof(*process));
        return status;
    }

    wchar_t *app = utf8_to_wide(options->argv[0], err);
    wchar_t *cwd = options->cwd ? utf8_to_wide(options->cwd, err) : NULL;
    size_t env_chars = 0;
    wchar_t *env = options->env ? build_env_block(options->env, &env_chars, err) : NULL;
    nb_buf command;
    nb_buf_init(&command);
    bool command_ok = false;
    wchar_t *command_wide = NULL;
    if (app) {
        if (build_command_line(options->argv, &command)) {
            command_wide = utf8_to_wide(nb_buf_cstr(&command), err);
            command_ok = command_wide != NULL;
        } else {
            nb_err_set(err, NB_ERR_NOMEM,
                       "out of memory building command line");
        }
    }
    bool setup_ok =
        command_ok && command_wide && (!options->cwd || cwd) &&
        (!options->env || env);
    nb_buf_destroy(&command);

    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    if (options->new_group) {
        status = create_job(&state.job, err);
        if (status != NB_OK) {
            setup_ok = false;
        } else {
            flags |= CREATE_SUSPENDED;
        }
    }

    if (setup_ok) {
        STARTUPINFOW startup;
        memset(&startup, 0, sizeof(startup));
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = state.in_read;
        startup.hStdOutput = state.out_write;
        startup.hStdError =
            options->merge_stderr ? state.out_write : state.err_write;

        if (!CreateProcessW(options->search_path ? NULL : app, command_wide, NULL, NULL, TRUE,
                            flags, env, cwd, &startup, &state.info)) {
            status = set_win_error(err, GetLastError(), "CreateProcessW");
            setup_ok = false;
        } else {
            state.created = true;
        }
    }
    nb_free(command_wide,
            command_wide ? (wcslen(command_wide) + 1u) * sizeof(wchar_t) : 0);
    nb_free(env, env_chars * sizeof(*env));
    nb_free(cwd, cwd ? (wcslen(cwd) + 1u) * sizeof(wchar_t) : 0);
    nb_free(app, app ? (wcslen(app) + 1u) * sizeof(wchar_t) : 0);

    if (!setup_ok) {
        spawn_state_clear(&state);
        nb_free(process, sizeof(*process));
        return status == NB_OK && err && err->code != 0
                   ? (nb_status)err->code
                   : (status == NB_OK ? NB_ERR_OS : status);
    }

    /* The parent keeps only the pipe ends used by its own threads. */
    close_handle(&state.in_read);
    close_handle(&state.out_write);
    if (!options->merge_stderr) {
        close_handle(&state.err_write);
    }

    if (state.in_write)
        process->stdin_fd = adopt_handle(&state.in_write, err);
    if (state.out_read)
        process->stdout_fd = adopt_handle(&state.out_read, err);
    if (state.err_read) {
        process->stderr_fd = adopt_handle(&state.err_read, err);
    }
    if ((options->stdin_mode == NB_PROCESS_PIPE && process->stdin_fd < 0) ||
        (options->stdout_mode == NB_PROCESS_PIPE && process->stdout_fd < 0) ||
        (!options->merge_stderr && options->stderr_mode == NB_PROCESS_PIPE &&
         process->stderr_fd < 0)) {
        if (process->stdin_fd >= 0) {
            (void)_close(process->stdin_fd);
        }
        if (process->stdout_fd >= 0) {
            (void)_close(process->stdout_fd);
        }
        if (process->stderr_fd >= 0) {
            (void)_close(process->stderr_fd);
        }
        spawn_state_clear(&state);
        nb_free(process, sizeof(*process));
        return err && err->code != 0 ? (nb_status)err->code : NB_ERR_OS;
    }

    if (options->new_group) {
        if (!AssignProcessToJobObject(state.job, state.info.hProcess)) {
            status = set_win_error(err, GetLastError(),
                                   "AssignProcessToJobObject");
            setup_ok = false;
        } else if (ResumeThread(state.info.hThread) == (DWORD)-1) {
            status = set_win_error(err, GetLastError(), "ResumeThread");
            setup_ok = false;
        }
    }
    if (!setup_ok) {
        (void)_close(process->stdin_fd);
        (void)_close(process->stdout_fd);
        if (process->stderr_fd >= 0) {
            (void)_close(process->stderr_fd);
        }
        spawn_state_clear(&state);
        nb_free(process, sizeof(*process));
        return status;
    }

    process->process = state.info.hProcess;
    state.info.hProcess = NULL;
    process->job = state.job;
    state.job = NULL;
    process->running = true;
    process->exited = false;
    process->exit_code = 0;
    state.created = false;
    spawn_state_clear(&state);
    *out = process;
    return NB_OK;
}

static void update_exit(nb_process *process, DWORD code)
{
    process->exit_code = code;
    process->running = false;
    process->exited = true;
}

nb_status nb_platform_process_wait(nb_process *process, int timeout_ms,
                                   int *exit_code, bool *exited, nb_err *err)
{
    if (!process) {
        nb_err_set(err, NB_ERR_INVAL, "invalid process");
        return NB_ERR_INVAL;
    }
    if (!process->running) {
        if (exit_code) {
            *exit_code = (int)process->exit_code;
        }
        if (exited) {
            *exited = process->exited;
        }
        return NB_OK;
    }

    DWORD wait = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
    DWORD rc = WaitForSingleObject(process->process, wait);
    if (rc == WAIT_TIMEOUT) {
        if (exit_code) {
            *exit_code = 0;
        }
        if (exited) {
            *exited = false;
        }
        nb_err_set(err, NB_ERR_TIMEOUT, "process is still running");
        return NB_ERR_TIMEOUT;
    }
    if (rc == WAIT_FAILED) {
        return set_win_error(err, GetLastError(), "WaitForSingleObject");
    }

    DWORD code = 0;
    if (!GetExitCodeProcess(process->process, &code)) {
        return set_win_error(err, GetLastError(), "GetExitCodeProcess");
    }
    update_exit(process, code);
    if (exit_code) {
        *exit_code = (int)code;
    }
    if (exited) {
        *exited = true;
    }
    return NB_OK;
}

nb_status nb_platform_process_signal(nb_process *process, int signal,
                                     nb_err *err)
{
    if (!process) {
        nb_err_set(err, NB_ERR_INVAL, "invalid process");
        return NB_ERR_INVAL;
    }
    if (!process->running) {
        nb_err_set(err, NB_ERR_STATE, "process already exited");
        return NB_ERR_STATE;
    }
    if (signal != SIGTERM && signal != SIGKILL) {
        nb_err_set(err, NB_ERR_UNSUPPORTED,
                   "Windows only maps SIGTERM/SIGKILL to termination");
        return NB_ERR_UNSUPPORTED;
    }

    BOOL ok = process->job
                  ? TerminateJobObject(process->job, 1)
                  : TerminateProcess(process->process, 1);
    if (!ok) {
        return set_win_error(err, GetLastError(), "terminate process");
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

    (void)(process->job ? TerminateJobObject(process->job, 1)
                        : TerminateProcess(process->process, 1));
    DWORD wait = grace_ms < 0 ? INFINITE : (DWORD)grace_ms;
    if (WaitForSingleObject(process->process, wait) == WAIT_TIMEOUT) {
        (void)TerminateProcess(process->process, 1);
        (void)WaitForSingleObject(process->process, INFINITE);
    }

    DWORD code = 0;
    if (GetExitCodeProcess(process->process, &code)) {
        update_exit(process, code);
    } else {
        process->running = false;
        process->exited = true;
    }
    return NB_OK;
}

static nb_status write_all_fd(int fd, const void *data, size_t len,
                              nb_err *err)
{
    const unsigned char *ptr = data;
    size_t left = len;
    while (left > 0) {
        unsigned int chunk = left > (size_t)INT_MAX
                                 ? (unsigned int)INT_MAX
                                 : (unsigned int)left;
        int n = _write(fd, ptr, chunk);
        if (n < 0) {
            int saved = errno;
            nb_status status =
                saved == EAGAIN ? NB_ERR_AGAIN : NB_ERR_IO;
            nb_err_set_os(err, status, saved, "write to stdin failed: %s",
                          strerror(saved));
            return status;
        }
        if (n == 0) {
            nb_err_set(err, NB_ERR_IO, "short write to stdin");
            return NB_ERR_IO;
        }
        ptr += (size_t)n;
        left -= (size_t)n;
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
    return write_all_fd(process->stdin_fd, data, len, err);
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
    /* _close also closes the HANDLE adopted by _open_osfhandle. */
    if (_close(fd) != 0) {
        int saved = errno;
        nb_err_set_os(err, NB_ERR_IO, saved, "close stdin failed: %s",
                      strerror(saved));
        return NB_ERR_IO;
    }
    return NB_OK;
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
    intptr_t os_handle = _get_osfhandle(fd);
    if (os_handle == -1) {
        nb_err_set_os(err, NB_ERR_INVAL, EBADF, "invalid CRT fd %d", fd);
        return NB_ERR_INVAL;
    }
    HANDLE handle = (HANDLE)os_handle;

    int64_t deadline = 0;
    if (timeout_ms >= 0) {
        deadline = nb_platform_time_monotonic_ns() +
                   (int64_t)timeout_ms * INT64_C(1000000);
    }

    unsigned char chunk[NB_PROCESS_CHUNK];
    size_t total = 0;
    for (;;) {
        /* WaitForSingleObject is not a readiness test for byte-mode pipe
         * handles; it may report the handle as signaled while _read blocks.
         * PeekNamedPipe is the supported way to poll an anonymous pipe. */
        DWORD available = 0;
        if (!PeekNamedPipe(handle, NULL, 0, NULL, &available, NULL)) {
            DWORD code = GetLastError();
            if (code == ERROR_BROKEN_PIPE) {
                if (total == 0) {
                    *read_out = 0;
                    return NB_OK;
                }
                break;
            }
            return set_win_error(err, code, "PeekNamedPipe");
        }

        if (available > 0) {
            size_t want = NB_MIN(max_bytes - total, sizeof(chunk));
            int n = _read(fd, chunk, (unsigned int)want);
            if (n < 0) {
                int saved = errno;
                if (saved == EINTR) {
                    continue;
                }
                if (saved == EAGAIN) {
                    if (total > 0) {
                        break;
                    }
                    nb_err_set(err, NB_ERR_TIMEOUT,
                               "no output within %d ms", timeout_ms);
                    return NB_ERR_TIMEOUT;
                }
                nb_err_set_os(err, NB_ERR_IO, saved, "_read failed: %s",
                              strerror(saved));
                return NB_ERR_IO;
            }
            if (n == 0) {
                break;
            }
            if (!nb_buf_append(out, chunk, (size_t)n)) {
                nb_err_set(err, NB_ERR_NOMEM,
                           "out of memory buffering output");
                return NB_ERR_NOMEM;
            }
            total += (size_t)n;
            if (total >= max_bytes) {
                break;
            }
            continue;
        }

        if (timeout_ms == 0) {
            if (total > 0) {
                break;
            }
            nb_err_set(err, NB_ERR_TIMEOUT, "no output within 0 ms");
            return NB_ERR_TIMEOUT;
        }
        if (timeout_ms > 0) {
            int64_t now = nb_platform_time_monotonic_ns();
            if (now >= deadline) {
                if (total > 0) {
                    break;
                }
                nb_err_set(err, NB_ERR_TIMEOUT, "no output within %d ms",
                           timeout_ms);
                return NB_ERR_TIMEOUT;
            }
            int64_t remain_ms = (deadline - now) / INT64_C(1000000);
            if (remain_ms < 1) {
                remain_ms = 1;
            }
            if (remain_ms > 10) {
                remain_ms = 10;
            }
            Sleep((DWORD)remain_ms);
        } else {
            Sleep(10);
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
        (void)(process->job ? TerminateJobObject(process->job, 1)
                            : TerminateProcess(process->process, 1));
        (void)WaitForSingleObject(process->process, INFINITE);
        process->running = false;
        process->exited = true;
    }
    close_handle(&process->job);
    close_handle(&process->process);
    if (process->stdin_fd >= 0) {
        (void)_close(process->stdin_fd);
    }
    if (process->stdout_fd >= 0) {
        (void)_close(process->stdout_fd);
    }
    if (process->stderr_fd >= 0) {
        (void)_close(process->stderr_fd);
    }
    nb_free(process, sizeof(*process));
}

int64_t nb_platform_process_id(const nb_process *process)
{
    return process ? (int64_t)GetProcessId(process->process) : 0;
}
int nb_platform_process_exit_signal(const nb_process *process)
{
    (void)process;
    return 0;
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

struct exchange_writer {
    nb_process *process;
    const void *input;
    size_t len;
    int64_t deadline;
    nb_atomic_int stop;
    nb_atomic_int done;
    nb_status status;
};

static int exchange_stopped(void *opaque)
{
    struct exchange_writer *writer = opaque;
    return nb_atomic_int_load(&writer->stop);
}

static void *exchange_write(void *opaque)
{
    struct exchange_writer *writer = opaque;
    writer->status =
        writer->process->stdin_fd < 0
            ? (writer->len ? NB_ERR_BROKEN_PIPE : NB_OK)
            : nb_platform_fd_write_until(writer->process->stdin_fd, writer->input, writer->len,
                                         writer->deadline, exchange_stopped, writer, NULL);
    (void)nb_platform_process_close_stdin(writer->process, NULL);
    nb_atomic_int_store(&writer->done, 1);
    return NULL;
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
    int64_t deadline = options->timeout_ms < 0
                           ? INT64_MAX
                           : nb_platform_time_monotonic_ns() + (int64_t)budget * 1000000;
    struct exchange_writer writer = {
        .process = process, .input = input, .len = len, .deadline = deadline};
    nb_atomic_int_init(&writer.stop, 0);
    nb_atomic_int_init(&writer.done, 0);
    nb_thread *thread = NULL;
    nb_status status = nb_thread_create(&thread, exchange_write, &writer, NULL);
    if (status != NB_OK)
        return status;
    bool open[] = {process->stdout_fd >= 0, process->stderr_fd >= 0};
    while (status == NB_OK && (process->running || open[0] || open[1])) {
        if (nb_atomic_int_load(&writer.done) && writer.status != NB_OK) {
            status = writer.status;
            break;
        }
        if (options->cancelled && options->cancelled(options->cancel_ctx)) {
            status = NB_ERR_CANCELLED;
            break;
        }
        if (nb_platform_time_monotonic_ns() >= deadline) {
            status = NB_ERR_TIMEOUT;
            break;
        }
        for (int i = 0; i < 2 && status == NB_OK; i++) {
            if (!open[i])
                continue;
            nb_buf chunk = {0};
            size_t got = 0;
            status = nb_platform_process_read(process, i != 0, &chunk, 16384, 0, &got, err);
            if (status == NB_ERR_TIMEOUT)
                status = NB_OK;
            else if (status == NB_OK && !got)
                open[i] = false;
            if (status == NB_OK && got) {
                nb_buf *out = i ? error : output;
                size_t keep = nb_buf_len(out) < options->max_output
                                  ? options->max_output - nb_buf_len(out)
                                  : 0;
                if (keep > got)
                    keep = got;
                bool *truncated = i ? options->stderr_truncated : options->stdout_truncated;
                if (keep < got && truncated)
                    *truncated = true;
                if (!nb_buf_append(out, nb_buf_cstr(&chunk), keep))
                    status = NB_ERR_NOMEM;
            }
            nb_buf_destroy(&chunk);
        }
        if (status != NB_OK)
            break;
        bool exited = false;
        status = nb_platform_process_wait(process, 0, exit_code, &exited, err);
        if (status == NB_ERR_TIMEOUT)
            status = NB_OK;
        Sleep(1);
    }
    nb_atomic_int_store(&writer.stop, 1);
    (void)nb_thread_join_take(&thread, NULL);
    (void)nb_platform_process_close_stdin(process, NULL);
    if (status == NB_OK)
        status = writer.status == NB_ERR_CANCELLED ? NB_ERR_BROKEN_PIPE : writer.status;
    if (status != NB_OK) {
        (void)nb_platform_process_terminate(process, 0);
        (void)nb_platform_process_wait(process, -1, exit_code, NULL, NULL);
        nb_err_set(err, status, "%s", nb_status_str(status));
    } else {
        nb_err_clear(err);
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
