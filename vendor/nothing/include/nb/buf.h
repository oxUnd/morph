#ifndef NB_BUF_H
#define NB_BUF_H

#include "nb/base.h"
#include "nb/err.h"
#include "nb/str.h"

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Growable byte buffer. off is the start of the unread region; the visible
 * content is [ptr + off, ptr + len). len excludes the trailing NUL and
 * ptr[len] is always kept at '\0'. cap is the allocated size in bytes,
 * including room for the trailing NUL. Consuming bytes only advances off;
 * call nb_buf_compact() (or let nb_buf_reserve() do it) to reclaim the
 * consumed prefix.
 */
typedef struct nb_buf {
    char *ptr;
    size_t len;
    size_t cap;
    size_t off;
    /* Sticky allocation/formatting failure for chaining operations. Cleared
     * by nb_buf_clear() or nb_buf_clear_error(). */
    bool error;
    nb_status error_code;
} nb_buf;

NB_EXPORT nb_buf *nb_buf_new(void);
NB_EXPORT nb_buf *nb_buf_new_with_cap(size_t cap);
NB_EXPORT void nb_buf_free(nb_buf *buf);

/* Embedded-buffer lifecycle. nb_buf_init() zeroes a caller-owned handle;
 * nb_buf_destroy() releases its storage but not the handle itself. */
NB_EXPORT void nb_buf_init(nb_buf *buf);
NB_EXPORT void nb_buf_destroy(nb_buf *buf);

/* Swaps the two handles. src receives dst's previous storage and remains
 * usable; no bytes are copied. */
NB_EXPORT void nb_buf_move(nb_buf *dst, nb_buf *src);

NB_EXPORT size_t nb_buf_len(const nb_buf *buf);
NB_EXPORT bool nb_buf_empty(const nb_buf *buf);
NB_EXPORT const char *nb_buf_cstr(const nb_buf *buf);
NB_EXPORT nb_str nb_buf_str(const nb_buf *buf);
NB_EXPORT bool nb_buf_ok(const nb_buf *buf);
/* First failure is sticky until clear/clear_error. Direct error=true without
 * a specific code reports NB_ERR_NOMEM. NULL reports NB_ERR_INVAL. */
NB_EXPORT nb_status nb_buf_status(const nb_buf *buf);
NB_EXPORT void nb_buf_set_error(nb_buf *buf, nb_status status);
NB_EXPORT void nb_buf_clear_error(nb_buf *buf);

/* Ensures room for additional bytes plus the trailing NUL. A consumed prefix
 * is compacted once it reaches half the capacity. Smaller prefixes may cause
 * geometric growth to avoid moving the live contents on every small append. */
NB_EXPORT bool nb_buf_reserve(nb_buf *buf, size_t additional);
/* Shrinks capacity to the visible bytes plus the trailing NUL, compacting a
 * consumed prefix first. Returns false if the backing allocation cannot be
 * resized; the original storage and contents are then left intact. */
NB_EXPORT bool nb_buf_shrink(nb_buf *buf);
/* data may refer to initialized bytes in this buffer, including consumed
 * bytes. Such input is preserved across growth and compaction. */
NB_EXPORT bool nb_buf_append(nb_buf *buf, const void *data, size_t len);
NB_EXPORT bool nb_buf_append_cstr(nb_buf *buf, const char *s);
NB_EXPORT bool nb_buf_append_str(nb_buf *buf, nb_str s);
NB_EXPORT bool nb_buf_append_char(nb_buf *buf, char c);

/* Appends value as base-10 digits without using the current locale. Returns
 * false and leaves the buffer unchanged if it cannot grow. */
NB_EXPORT bool nb_buf_append_i64(nb_buf *buf, int64_t value);
NB_EXPORT bool nb_buf_append_u64(nb_buf *buf, uint64_t value);

NB_EXPORT bool nb_buf_printf(nb_buf *buf, const char *fmt, ...) NB_PRINTF(2, 3);
NB_EXPORT bool nb_buf_vprintf(nb_buf *buf, const char *fmt, va_list ap);

/* Marks n bytes at the front as consumed. This only advances off; the
 * remainder is not moved and existing views stay valid until append/compact.
 * Consuming all visible bytes leaves off == len rather than clearing. */
NB_EXPORT void nb_buf_consume(nb_buf *buf, size_t n);

/* Moves unread bytes to the start of the allocation. */
NB_EXPORT void nb_buf_compact(nb_buf *buf);

/* Shrinks the absolute end to len bytes and keeps the trailing NUL. Does
 * nothing when len >= buf->len; clears visible content when len <= off. */
NB_EXPORT void nb_buf_truncate(nb_buf *buf, size_t len);

NB_EXPORT void nb_buf_clear(nb_buf *buf);

/* Transfers the backing allocation to the caller. On success buf is empty and
 * *cap_out receives the allocation size required by nb_free(). Returns NULL
 * for an empty buffer or invalid input. */
NB_EXPORT char *nb_buf_detach(nb_buf *buf, size_t *cap_out);

/* Reads once from fd and appends the bytes. max is the maximum to read.
 * Returns NB_OK with *read_out == 0 at EOF, NB_ERR_AGAIN when a non-blocking
 * fd has no data, or an I/O error. */
NB_EXPORT nb_status nb_buf_read_fd(nb_buf *buf, int fd, size_t max,
                                   size_t *read_out, nb_err *err);

/* Borrows the next complete line without the trailing newline or CRLF and
 * consumes it. Returns false when no complete line is buffered. */
NB_EXPORT bool nb_buf_next_line(nb_buf *buf, nb_str *line);

#ifdef __cplusplus
}
#endif

#endif /* NB_BUF_H */
