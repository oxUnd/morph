#ifndef NB_PLATFORM_H
#define NB_PLATFORM_H

#include "nb/base.h"
#include "nb/buf.h"
#include "nb/err.h"
#include "nb/poll.h"
#include "nb/process.h"

#include <time.h>

/* Internal platform hooks used by the portable path helpers. */
bool nb_platform_path_is_sep(char c);
bool nb_platform_path_is_absolute(const char *path);
size_t nb_platform_path_root_len(const char *path);
size_t nb_platform_path_drive_len(const char *path);
nb_status nb_platform_getcwd(nb_buf *out, nb_err *err);
nb_status nb_platform_file_read_all(const char *path, nb_buf *out, size_t max_bytes, nb_err *err);
nb_status nb_platform_path_canonical(nb_buf *out, const char *path, nb_err *err);
bool nb_platform_path_prefix_equal(const char *a, const char *b, size_t len);
nb_status nb_platform_path_executable(nb_buf *out, nb_err *err);
nb_status nb_platform_path_temp_dir(nb_buf *out, nb_err *err);

/* Internal fd/random/readiness hooks. */
nb_status nb_platform_fd_read(int fd, void *data, size_t len, size_t *read,
                              nb_err *err);
nb_status nb_platform_random_bytes(void *out, size_t len, nb_err *err);
nb_status nb_platform_poll(nb_poll_item *items, size_t count, int timeout_ms,
                           size_t *ready, nb_err *err);

nb_status nb_platform_pipe_open(int descriptors[2], nb_err *err);
nb_status nb_platform_fd_write_until(int fd, const void *data, size_t len, int64_t deadline_ns,
                                     int (*cancelled)(void *), void *ctx, nb_err *err);

/* Process hooks; the platform owns struct nb_process. */
nb_status nb_platform_process_spawn(const nb_process_options *options,
                                    nb_process **out, nb_err *err);
nb_status nb_platform_process_exchange(nb_process *process, const void *input, size_t len,
                                       const nb_process_exchange_options *options, nb_buf *output,
                                       nb_buf *error, int *exit_code, nb_err *err);
nb_status nb_platform_process_wait(nb_process *process, int timeout_ms,
                                   int *exit_code, bool *exited, nb_err *err);
nb_status nb_platform_process_signal(nb_process *process, int signal,
                                     nb_err *err);
nb_status nb_platform_process_terminate(nb_process *process, int grace_ms);
nb_status nb_platform_process_stdin_write(nb_process *process,
                                          const void *data, size_t len,
                                          nb_err *err);
nb_status nb_platform_process_stdin_write_until(nb_process *process, const void *data, size_t len,
                                                int64_t deadline_ns, int (*cancelled)(void *),
                                                void *ctx, nb_err *err);
nb_status nb_platform_process_close_stdin(nb_process *process, nb_err *err);
nb_status nb_platform_process_read(nb_process *process, bool stderr_stream,
                                   nb_buf *out, size_t max_bytes,
                                   int timeout_ms, size_t *read_out,
                                   nb_err *err);
int64_t nb_platform_process_id(const nb_process *process);
int nb_platform_process_exit_signal(const nb_process *process);
int nb_platform_process_take_stdin(nb_process *process);
int nb_platform_process_take_stdout(nb_process *process);
int nb_platform_process_take_stderr(nb_process *process);
int nb_platform_process_stdin_fd(const nb_process *process);
int nb_platform_process_stdout_fd(const nb_process *process);
int nb_platform_process_stderr_fd(const nb_process *process);
bool nb_platform_process_running(const nb_process *process);
void nb_platform_process_free(nb_process *process);

/* Internal platform hooks used by src/time.c. */
int64_t nb_platform_time_realtime_ns(void);
int64_t nb_platform_time_monotonic_ns(void);
void nb_platform_sleep_ms(uint32_t ms);
bool nb_platform_localtime(int64_t seconds, struct tm *out);
bool nb_platform_local_offset_seconds(int64_t seconds, int32_t *out);
bool nb_platform_gmtime(int64_t seconds, struct tm *out);

/* Process-wide default logger configuration lock; requires no allocation. */
void nb_platform_log_lock(void);
void nb_platform_log_unlock(void);

#endif /* NB_PLATFORM_H */
