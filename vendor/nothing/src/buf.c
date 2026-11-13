#include "nb/buf.h"

#include "nb/mem.h"
#include "container_internal.h"
#include "platform/nb_platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void nb_buf_init(nb_buf *buf)
{
    if (!buf) {
        return;
    }
    buf->ptr = NULL;
    buf->len = 0;
    buf->cap = 0;
    buf->off = 0;
    buf->error = false;
    buf->error_code = NB_OK;
}

void nb_buf_destroy(nb_buf *buf)
{
    if (!buf) {
        return;
    }
    if (buf->ptr) {
        nb_free(buf->ptr, buf->cap);
    }
    nb_buf_init(buf);
}

void nb_buf_move(nb_buf *dst, nb_buf *src)
{
    if (!dst || !src || dst == src) {
        return;
    }
    nb_buf tmp = *dst;
    *dst = *src;
    *src = tmp;
}

nb_buf *nb_buf_new(void)
{
    nb_buf *buf = nb_try_alloc(sizeof(*buf));
    if (!buf) {
        return NULL;
    }
    nb_buf_init(buf);
    return buf;
}

nb_buf *nb_buf_new_with_cap(size_t cap)
{
    nb_buf *buf = nb_buf_new();
    if (cap != 0 && !nb_buf_reserve(buf, cap)) {
        nb_buf_free(buf);
        return NULL;
    }
    return buf;
}

void nb_buf_free(nb_buf *buf)
{
    if (!buf) {
        return;
    }
    nb_buf_destroy(buf);
    nb_free(buf, sizeof(*buf));
}

size_t nb_buf_len(const nb_buf *buf)
{
    return buf && buf->off <= buf->len ? buf->len - buf->off : 0u;
}

bool nb_buf_empty(const nb_buf *buf)
{
    return !buf || buf->off > buf->len || buf->len == buf->off;
}

const char *nb_buf_cstr(const nb_buf *buf)
{
    if (!buf || !buf->ptr) {
        return "";
    }
    return buf->ptr + (buf->off <= buf->len ? buf->off : buf->len);
}

nb_str nb_buf_str(const nb_buf *buf)
{
    nb_str str;
    str.ptr = nb_buf_cstr(buf);
    str.len = nb_buf_len(buf);
    return str;
}

bool nb_buf_ok(const nb_buf *buf)
{
    return buf != NULL && !buf->error;
}

nb_status nb_buf_status(const nb_buf *buf)
{
    if (!buf) {
        return NB_ERR_INVAL;
    }
    return !buf->error ? NB_OK : buf->error_code < NB_OK ? buf->error_code : NB_ERR_NOMEM;
}

void nb_buf_set_error(nb_buf *buf, nb_status status)
{
    if (buf && !buf->error && status < NB_OK) {
        buf->error = true;
        buf->error_code = status;
    }
}

void nb_buf_clear_error(nb_buf *buf)
{
    if (buf) {
        buf->error = false;
        buf->error_code = NB_OK;
    }
}

bool nb_buf_reserve(nb_buf *buf, size_t additional)
{
    if (!buf) {
        return false;
    }
    if (buf->off > buf->len) {
        nb_buf_set_error(buf, NB_ERR_NOMEM);
        return false;
    }
    if (buf->error) {
        return false;
    }
    size_t visible = buf->len - buf->off;
    if (additional >= SIZE_MAX - visible) {
        nb_buf_set_error(buf, NB_ERR_NOMEM);
        return false;
    }
    bool compact = buf->off != 0 && buf->off >= buf->cap / 2u;
    size_t base = compact ? visible : buf->len;
    if (additional >= SIZE_MAX - base) {
        nb_buf_set_error(buf, NB_ERR_NOMEM);
        return false;
    }
    size_t needed_abs = base + additional + 1u;

    /* Small consumed prefixes do not justify moving all live bytes. Grow
     * geometrically until compaction can be amortized over enough consumes. */
    if (needed_abs > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap : 16u;
        while (new_cap < needed_abs) {
            if (new_cap > SIZE_MAX / 2u) {
                new_cap = needed_abs;
                break;
            }
            new_cap *= 2u;
        }
        char *ptr = nb_try_realloc(buf->ptr, buf->cap, new_cap);
        if (!ptr) {
            nb_buf_set_error(buf, NB_ERR_NOMEM);
            return false;
        }
        buf->ptr = ptr;
        buf->cap = new_cap;
        if (buf->len == 0) {
            buf->ptr[0] = '\0';
        }
    }

    /* Reclaim the consumed prefix only after capacity is known to be
     * sufficient, so a failed growth leaves the buffer untouched. */
    if (compact) {
        nb_buf_compact(buf);
    }
    return true;
}

bool nb_buf_shrink(nb_buf *buf)
{
    if (!buf) {
        return false;
    }
    if (buf->off > buf->len) {
        nb_buf_set_error(buf, NB_ERR_NOMEM);
        return false;
    }
    if (buf->error) {
        return false;
    }
    size_t visible = buf->len - buf->off;
    if (visible == SIZE_MAX) {
        return false;
    }
    size_t needed = visible + 1u;
    if (needed >= buf->cap) {
        return true;
    }

    /* Compaction before a fallible realloc would invalidate existing views
     * even when shrink fails. Stage a smaller allocation for consumed data. */
    char *ptr = buf->off != 0 ? nb_try_alloc(needed)
                              : nb_try_realloc(buf->ptr, buf->cap, needed);
    if (!ptr) {
        return false;
    }
    if (buf->off != 0) {
        memcpy(ptr, buf->ptr + buf->off, needed);
        nb_free(buf->ptr, buf->cap);
    }
    buf->ptr = ptr;
    buf->len = visible;
    buf->off = 0;
    buf->cap = needed;
    return true;
}

bool nb_buf_append(nb_buf *buf, const void *data, size_t len)
{
    if (!buf || (!data && len != 0)) {
        return false;
    }
    if (buf->error) {
        return false;
    }
    size_t offset = 0;
    bool internal = len != 0 &&
        nb_span_offset(buf->ptr, buf->len == SIZE_MAX ? SIZE_MAX : buf->len + 1u,
                        data, len, &offset);
    void *copy = NULL;
    size_t old_off = buf->off;
    if (internal && offset < old_off) {
        copy = nb_try_alloc(len);
        if (!copy) {
            nb_buf_set_error(buf, NB_ERR_NOMEM);
            return false;
        }
        memcpy(copy, data, len);
        data = copy;
    }
    if (!nb_buf_reserve(buf, len)) {
        nb_free(copy, len);
        return false;
    }
    if (internal && !copy) {
        data = buf->ptr + offset - (old_off - buf->off);
    }
    if (len != 0) {
        memmove(buf->ptr + buf->len, data, len);
        buf->len += len;
    }
    buf->ptr[buf->len] = '\0';
    nb_free(copy, len);
    return true;
}

bool nb_buf_append_cstr(nb_buf *buf, const char *s)
{
    return s ? nb_buf_append(buf, s, strlen(s)) : false;
}

bool nb_buf_append_str(nb_buf *buf, nb_str s)
{
    return nb_buf_append(buf, s.ptr, s.len);
}

bool nb_buf_append_char(nb_buf *buf, char c)
{
    return nb_buf_append(buf, &c, 1u);
}

/* Writes value as decimal digits into out and returns the digit count. out
 * must have room for 20 bytes. */
static size_t nb_format_u64(char *out, uint64_t value)
{
    char reversed[20];
    size_t len = 0;
    do {
        reversed[len++] = (char)('0' + (int)(value % 10u));
        value /= 10u;
    } while (value != 0);
    for (size_t i = 0; i < len; i++) {
        out[i] = reversed[len - 1u - i];
    }
    return len;
}

bool nb_buf_append_u64(nb_buf *buf, uint64_t value)
{
    char digits[20];
    size_t len = nb_format_u64(digits, value);
    return nb_buf_append(buf, digits, len);
}

bool nb_buf_append_i64(nb_buf *buf, int64_t value)
{
    char tmp[21];
    size_t len = 0;
    bool negative = value < 0;
    /* Taking the magnitude through the unsigned domain is defined for
     * INT64_MIN, unlike -value. */
    uint64_t magnitude = negative ? (uint64_t)0 - (uint64_t)value
                                  : (uint64_t)value;
    if (negative) {
        tmp[len++] = '-';
    }
    len += nb_format_u64(tmp + len, magnitude);
    return nb_buf_append(buf, tmp, len);
}

bool nb_buf_vprintf(nb_buf *buf, const char *fmt, va_list ap)
{
    if (!buf || !fmt) {
        return false;
    }
    if (buf->error) {
        return false;
    }
    char stack[256];
    va_list copy;
    va_copy(copy, ap);
    int needed = vsnprintf(stack, sizeof(stack), fmt, copy);
    va_end(copy);
    if (needed < 0) {
        nb_buf_set_error(buf, NB_ERR_FORMAT);
        return false;
    }
    size_t n = (size_t)needed;
    char *formatted = stack;
    if (n >= sizeof(stack)) {
        formatted = nb_try_alloc(n + 1u);
        if (!formatted) {
            nb_buf_set_error(buf, NB_ERR_NOMEM);
            return false;
        }
        int written = vsnprintf(formatted, n + 1u, fmt, ap);
        if (written < 0 || (size_t)written != n) {
            nb_free(formatted, n + 1u);
            nb_buf_set_error(buf, NB_ERR_NOMEM);
            return false;
        }
    }
    /* Stage formatting before growth: fmt and %s arguments can borrow buf. */
    bool ok = nb_buf_append(buf, formatted, n);
    if (formatted != stack) {
        nb_free(formatted, n + 1u);
    }
    return ok;
}

bool nb_buf_printf(nb_buf *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bool ok = nb_buf_vprintf(buf, fmt, ap);
    va_end(ap);
    return ok;
}

void nb_buf_consume(nb_buf *buf, size_t n)
{
    if (!buf || n == 0 || buf->off > buf->len) {
        return;
    }
    if (n >= buf->len - buf->off) {
        buf->off = buf->len;
        return;
    }
    buf->off += n;
}

void nb_buf_compact(nb_buf *buf)
{
    if (!buf || buf->off == 0) {
        return;
    }
    if (buf->off > buf->len) {
        buf->len = 0;
        buf->off = 0;
        if (buf->ptr) {
            buf->ptr[0] = '\0';
        }
        return;
    }
    size_t visible = buf->len - buf->off;
    if (buf->ptr) {
        if (visible != 0) {
            memmove(buf->ptr, buf->ptr + buf->off, visible);
        }
        buf->ptr[visible] = '\0';
    }
    buf->len = visible;
    buf->off = 0;
}

void nb_buf_truncate(nb_buf *buf, size_t len)
{
    if (!buf || len >= buf->len) {
        return;
    }
    if (len <= buf->off) {
        buf->len = 0;
        buf->off = 0;
        if (buf->ptr) {
            buf->ptr[0] = '\0';
        }
        return;
    }
    buf->len = len;
    if (buf->ptr) {
        buf->ptr[len] = '\0';
    }
}

void nb_buf_clear(nb_buf *buf)
{
    if (!buf) {
        return;
    }
    buf->len = 0;
    buf->off = 0;
    buf->error = false;
    buf->error_code = NB_OK;
    if (buf->ptr) {
        buf->ptr[0] = '\0';
    }
}

char *nb_buf_detach(nb_buf *buf, size_t *cap_out)
{
    if (!buf || !cap_out || !buf->ptr || buf->off > buf->len) {
        return NULL;
    }
    if (buf->len - buf->off == 0) {
        return NULL;
    }

    /* Compact first so the caller receives only the visible bytes. */
    nb_buf_compact(buf);
    char *ptr = buf->ptr;
    size_t cap = buf->cap;
    nb_buf_init(buf);
    *cap_out = cap;
    return ptr;
}

nb_status nb_buf_read_fd(nb_buf *buf, int fd, size_t max, size_t *read_out,
                         nb_err *err)
{
    if (read_out) {
        *read_out = 0;
    }
    if (!buf || fd < 0) {
        nb_err_set(err, NB_ERR_INVAL, "invalid buffer or file descriptor");
        return NB_ERR_INVAL;
    }
    if (max == 0) {
        return NB_OK;
    }
    /* Stage the read so EAGAIN and I/O errors leave the buffer untouched.
     * Small reads use the stack; larger maximums allocate exactly max. */
    char stack[4096];
    char *scratch = stack;
    if (max > sizeof(stack)) {
        scratch = nb_try_alloc(max);
        if (!scratch) {
            nb_err_set(err, NB_ERR_NOMEM, "no space to stage fd read");
            return NB_ERR_NOMEM;
        }
    }

    size_t read = 0;
    nb_status status = nb_platform_fd_read(fd, scratch, max, &read, err);
    if (status == NB_OK && read > max) {
        nb_err_set(err, NB_ERR_IO, "fd read reported an impossible length");
        status = NB_ERR_IO;
    }
    if (status == NB_OK && read != 0 &&
        !nb_buf_append(buf, scratch, read)) {
        nb_err_set(err, NB_ERR_NOMEM, "buffer cannot grow for fd read");
        status = NB_ERR_NOMEM;
    }
    if (scratch != stack) {
        nb_free(scratch, max);
    }
    if (status != NB_OK) {
        return status;
    }
    if (read_out) {
        *read_out = read;
    }
    return NB_OK;
}

bool nb_buf_next_line(nb_buf *buf, nb_str *line)
{
    if (!buf || !line || buf->off > buf->len || !buf->ptr) {
        return false;
    }
    const char *start = buf->ptr + buf->off;
    size_t visible = buf->len - buf->off;
    const char *newline = memchr(start, '\n', visible);
    if (!newline) {
        return false;
    }

    size_t consumed = (size_t)(newline - start) + 1u;
    size_t line_len = consumed - 1u;
    if (line_len != 0 && start[line_len - 1u] == '\r') {
        line_len--;
    }
    line->ptr = start;
    line->len = line_len;
    nb_buf_consume(buf, consumed);
    return true;
}
