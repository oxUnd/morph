#ifndef NB_PROCESS_H
#define NB_PROCESS_H

#include "nb/base.h"
#include "nb/buf.h"
#include "nb/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nb_process nb_process;

typedef enum nb_process_stream {
    NB_PROCESS_PIPE = 0,
    NB_PROCESS_INHERIT,
    NB_PROCESS_NULL,
    NB_PROCESS_FD
} nb_process_stream;

typedef struct nb_process_options {
    const char *const *argv; /* required; argv[0] names the executable and is
                              * resolved as a path, not through PATH */
    const char *cwd;         /* NULL inherits the current directory */
    const char *const *env;  /* NULL inherits; otherwise a complete "K=V" list */
    bool close_fds;          /* POSIX: close nonstandard inherited descriptors */
    bool search_path;        /* resolve argv[0] through the child's PATH */
    bool new_session;        /* POSIX: detach from controlling terminal */
    bool pty;                /* POSIX: one terminal for all three streams */
    nb_process_stream stdin_mode;
    nb_process_stream stdout_mode;
    nb_process_stream stderr_mode;
    int stdin_fd; /* borrowed when mode == NB_PROCESS_FD */
    int stdout_fd;
    int stderr_fd;
    /* POSIX-only sandbox hook, after stream setup and signal reset. The
     * callback must obey the platform's post-fork restrictions. Returning a
     * failure exits the child with status 126. The context is borrowed until
     * spawn returns. Unsupported platforms reject a non-NULL hook. */
    nb_status (*prepare)(void *ctx);
    void *prepare_ctx;
    bool merge_stderr;       /* stderr shares the stdout pipe */
    bool new_group;          /* POSIX: new process group; Windows: job object */
} nb_process_options;

NB_EXPORT nb_status nb_process_spawn(const nb_process_options *options,
                                     nb_process **out, nb_err *err);

/* timeout_ms < 0 waits forever; 0 polls once. On timeout returns
 * NB_ERR_TIMEOUT with *exited=false. exit_code is 128+signal on POSIX when
 * the child was signalled. */
typedef struct nb_process_exchange_options {
    int timeout_ms;    /* 0 uses five minutes; negative waits indefinitely */
    size_t max_output; /* retained bytes per stream; remainder drained */
    int (*cancelled)(void *ctx);
    void *cancel_ctx;
    bool *stdout_truncated;
    bool *stderr_truncated;
} nb_process_exchange_options;

/* Concurrently sends stdin and drains both outputs, closes stdin at EOF,
 * then reaps the child. On timeout, cancellation or I/O failure terminates
 * the process group and reaps the child. Existing output is appended to and
 * remains owned by the caller even on failure. exit_code is initialized on
 * every call and uses the same convention as nb_process_wait(). */
NB_EXPORT nb_status nb_process_exchange(nb_process *process, const void *input, size_t len,
                                        const nb_process_exchange_options *options, nb_buf *output,
                                        nb_buf *error, int *exit_code, nb_err *err);

NB_EXPORT nb_status nb_process_wait(nb_process *process, int timeout_ms,
                                    int *exit_code, bool *exited, nb_err *err);

/* POSIX signals are sent to the process group when new_group was requested.
 * Windows maps SIGTERM/SIGKILL to termination and rejects other signals. */
NB_EXPORT nb_status nb_process_signal(nb_process *process, int signal,
                                      nb_err *err);
NB_EXPORT nb_status nb_process_terminate(nb_process *process, int grace_ms);

/* Writes all bytes to the child's stdin. */
NB_EXPORT nb_status nb_process_stdin_write(nb_process *process,
                                           const void *data, size_t len,
                                           nb_err *err);

/* Cancellation-aware bounded write on a persistent process. deadline_ns is
 * absolute monotonic time. Restores the borrowed stream flags on return. */
NB_EXPORT nb_status nb_process_stdin_write_until(nb_process *process, const void *data, size_t len,
                                                 int64_t deadline_ns, int (*cancelled)(void *),
                                                 void *ctx, nb_err *err);

/* Closes the child's stdin pipe so readers observe EOF. Subsequent writes
 * return NB_ERR_STATE. Closing an already closed stream is a no-op. */
NB_EXPORT nb_status nb_process_close_stdin(nb_process *process, nb_err *err);

/* Reads up to max_bytes from stdout (or stderr), appending to out. Returns
 * NB_OK with *read_out == 0 at EOF, NB_ERR_TIMEOUT when no data arrives. */
NB_EXPORT nb_status nb_process_read(nb_process *process, bool stderr_stream,
                                    nb_buf *out, size_t max_bytes,
                                    int timeout_ms, size_t *read_out,
                                    nb_err *err);

/* Native identity for integration with process event protocols. */
NB_EXPORT int64_t nb_process_id(const nb_process *process);
NB_EXPORT int nb_process_exit_signal(const nb_process *process);
/* Transfer a stream descriptor to an external reactor. Once taken, that
 * descriptor belongs to the caller and is no longer closed by the process. */
NB_EXPORT int nb_process_take_stdin(nb_process *process);
NB_EXPORT int nb_process_take_stdout(nb_process *process);
NB_EXPORT int nb_process_take_stderr(nb_process *process);

NB_EXPORT int nb_process_stdin_fd(const nb_process *process);
NB_EXPORT int nb_process_stdout_fd(const nb_process *process);
NB_EXPORT int nb_process_stderr_fd(const nb_process *process);
NB_EXPORT bool nb_process_running(const nb_process *process);
/* Terminates a still-running child, reaps it, and releases all handles. */
NB_EXPORT void nb_process_free(nb_process *process);

#ifdef __cplusplus
}
#endif

#endif /* NB_PROCESS_H */
