#ifndef MORPH_CHILD_IO_H
#define MORPH_CHILD_IO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic milliseconds, suitable only for intervals within this process. */
int64_t morph_io_now_ms(void);
/* Creates two caller-owned close-on-exec fds. Failure leaves both at -1.
 * Linux creates the flags atomically; other hosts require spawn-side fd isolation. */
int morph_io_pipe(int descriptors[2]);
/* Borrowed fd/data/callback until return. Restores fd flags and signal mask.
 * deadline is absolute monotonic milliseconds; partial writes may occur on error. */
int morph_io_write(int fd, const char *data, size_t len, int64_t deadline, int (*cancelled)(void *),
		   void *user);
/* Owns the child until reaped, and consumes input_fd; output/error fds and buffers are borrowed.
 * Buffers must be initialized. Captures at most limit bytes per stream, draining
 * the remainder. On failure terminates the child/group, then reaps it.
 * status is initialized on every call. Callbacks run synchronously without locks. */
int morph_child_exchange(pid_t pid, int input_fd, int output_fd, int error_fd, const char *input,
			 size_t input_len, int timeout_ms, size_t limit, int (*cancelled)(void *),
			 void *user, morph_buf_t *output, morph_buf_t *error, int *status);

#ifdef __cplusplus
}
#endif
#endif
