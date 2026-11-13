#include "nb/process.h"

#include "platform/nb_platform.h"

nb_status nb_process_spawn(const nb_process_options *options, nb_process **out,
                           nb_err *err)
{
    if (!out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid process output");
        return NB_ERR_INVAL;
    }
    *out = NULL;
    if (!options || !options->argv || !options->argv[0] ||
        options->argv[0][0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "options and argv[0] are required");
        return NB_ERR_INVAL;
    }
    return nb_platform_process_spawn(options, out, err);
}

nb_status nb_process_wait(nb_process *process, int timeout_ms, int *exit_code,
                          bool *exited, nb_err *err)
{
    if (!process) {
        nb_err_set(err, NB_ERR_INVAL, "invalid process");
        return NB_ERR_INVAL;
    }
    return nb_platform_process_wait(process, timeout_ms, exit_code, exited,
                                    err);
}

nb_status nb_process_signal(nb_process *process, int signal, nb_err *err)
{
    if (!process) {
        nb_err_set(err, NB_ERR_INVAL, "invalid process");
        return NB_ERR_INVAL;
    }
    return nb_platform_process_signal(process, signal, err);
}

nb_status nb_process_terminate(nb_process *process, int grace_ms)
{
    if (!process) {
        return NB_ERR_INVAL;
    }
    return nb_platform_process_terminate(process, grace_ms);
}

nb_status nb_process_stdin_write(nb_process *process, const void *data,
                                 size_t len, nb_err *err)
{
    if (!process || (!data && len != 0)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid process or data");
        return NB_ERR_INVAL;
    }
    return nb_platform_process_stdin_write(process, data, len, err);
}

nb_status nb_process_close_stdin(nb_process *process, nb_err *err)
{
    if (!process) {
        nb_err_set(err, NB_ERR_INVAL, "invalid process");
        return NB_ERR_INVAL;
    }
    return nb_platform_process_close_stdin(process, err);
}

nb_status nb_process_read(nb_process *process, bool stderr_stream, nb_buf *out,
                          size_t max_bytes, int timeout_ms, size_t *read_out,
                          nb_err *err)
{
    if (read_out) {
        *read_out = 0;
    }
    if (!process || !out || !read_out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid process, buffer or output");
        return NB_ERR_INVAL;
    }
    return nb_platform_process_read(process, stderr_stream, out, max_bytes,
                                    timeout_ms, read_out, err);
}

int nb_process_stdin_fd(const nb_process *process)
{
    return process ? nb_platform_process_stdin_fd(process) : -1;
}

int nb_process_stdout_fd(const nb_process *process)
{
    return process ? nb_platform_process_stdout_fd(process) : -1;
}

int nb_process_stderr_fd(const nb_process *process)
{
    return process ? nb_platform_process_stderr_fd(process) : -1;
}

bool nb_process_running(const nb_process *process)
{
    return process ? nb_platform_process_running(process) : false;
}

void nb_process_free(nb_process *process)
{
    if (process) {
        nb_platform_process_free(process);
    }
}

int64_t nb_process_id(const nb_process *process) { return nb_platform_process_id(process); }
int nb_process_exit_signal(const nb_process *process)
{
    return nb_platform_process_exit_signal(process);
}
int nb_process_take_stdin(nb_process *process) { return nb_platform_process_take_stdin(process); }
int nb_process_take_stdout(nb_process *process) { return nb_platform_process_take_stdout(process); }
int nb_process_take_stderr(nb_process *process) { return nb_platform_process_take_stderr(process); }

nb_status nb_process_exchange(nb_process *process, const void *input, size_t len,
                              const nb_process_exchange_options *options, nb_buf *output,
                              nb_buf *error, int *exit_code, nb_err *err)
{
    if (exit_code)
        *exit_code = 0;
    if (!process || (!input && len) || !output || !error || !exit_code) {
        nb_err_set(err, NB_ERR_INVAL, "invalid process exchange arguments");
        return NB_ERR_INVAL;
    }
    return nb_platform_process_exchange(process, input, len, options, output, error, exit_code,
                                        err);
}

nb_status nb_process_stdin_write_until(nb_process *process, const void *data, size_t len,
                                       int64_t deadline_ns, int (*cancelled)(void *), void *ctx,
                                       nb_err *err)
{
    if (!process || (!data && len))
        return NB_ERR_INVAL;
    return nb_platform_process_stdin_write_until(process, data, len, deadline_ns, cancelled, ctx,
                                                 err);
}
