#ifndef NB_ERR_H
#define NB_ERR_H

#include "nb/base.h"

#include <stdarg.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_ERR_MSG_CAP 256

typedef enum nb_status {
    /* ABI bounds keep every signed application/user code a valid enum value
     * in both C and C++. They are bounds, not operation result statuses. */
    NB_STATUS_MIN = INT_MIN,
    NB_STATUS_MAX = INT_MAX,
    NB_OK = 0,
    NB_ERR_INVAL = -1,
    NB_ERR_NOMEM = -2,
    NB_ERR_IO = -3,
    NB_ERR_NOT_FOUND = -4,
    NB_ERR_EXISTS = -5,
    NB_ERR_RANGE = -6,
    NB_ERR_TIMEOUT = -7,
    NB_ERR_AGAIN = -8,
    NB_ERR_OS = -9,
    NB_ERR_PARSE = -10,
    NB_ERR_STATE = -11,
    NB_ERR_UNSUPPORTED = -12,
    NB_ERR_PERMISSION = -13,
    NB_ERR_CANCELLED = -14,
    NB_ERR_INTERRUPTED = -15,
    NB_ERR_TOO_BIG = -16,
    NB_ERR_BAD_HANDLE = -17,
    NB_ERR_BUSY = -18,
    NB_ERR_NO_CHILD = -19,
    NB_ERR_CONNECTION_REFUSED = -20,
    NB_ERR_CONNECTION_RESET = -21,
    NB_ERR_NO_SPACE = -22,
    NB_ERR_ENCODING = -23,
    NB_ERR_IN_PROGRESS = -24,
    NB_ERR_IS_DIR = -25,
    NB_ERR_LOOP = -26,
    NB_ERR_RESOURCE = -27,
    NB_ERR_NAME_TOO_LONG = -28,
    NB_ERR_NETWORK = -29,
    NB_ERR_NO_DATA = -30,
    NB_ERR_NOT_DIR = -31,
    NB_ERR_NOT_TERMINAL = -32,
    NB_ERR_BROKEN_PIPE = -33,
    NB_ERR_PROTOCOL = -34,
    NB_ERR_CLOSED = -35,
    NB_ERR_STALE = -36,
    NB_ERR_CROSS_DEVICE = -37,
    NB_ERR_FORMAT = -38,
    NB_ERR_NOT_PERMITTED = -39
} nb_status;

/*
 * Generic statuses are closed. Applications register domain errors in the
 * user range below; the numeric value is part of logs and wire protocols, so
 * number them explicitly instead of using __COUNTER__ or enum auto-values.
 * Modules should claim distinct blocks, e.g. NB_ERR_USER(100 + n).
 */
#define NB_ERR_USER_BASE (-1000)
#define NB_ERR_USER(n)   ((nb_status)(NB_ERR_USER_BASE - (int)(n)))
#define NB_ERR_IS_USER(c) ((int)(c) <= NB_ERR_USER_BASE)

typedef int nb_err_code;

typedef struct nb_err {
    nb_err_code code;
    int os_errno;
    char msg[NB_ERR_MSG_CAP];
} nb_err;


/* Static description for a user error code. All strings are borrowed. */
typedef struct nb_err_desc {
    nb_err_code code;
    const char *name;
    const char *message;
} nb_err_desc;

NB_EXPORT void nb_err_clear(nb_err *err);
NB_EXPORT void nb_err_set(nb_err *err, nb_err_code code, const char *fmt, ...)
    NB_PRINTF(3, 4);
NB_EXPORT void nb_err_setv(nb_err *err, nb_err_code code, const char *fmt,
                           va_list ap);
NB_EXPORT void nb_err_set_os(nb_err *err, nb_err_code code, int os_errno,
                             const char *fmt, ...) NB_PRINTF(4, 5);
NB_EXPORT const char *nb_status_str(nb_status status);
/* Maps a captured errno to a portable status. Zero maps to I/O failure,
 * never success. Optional err retains the exact OS diagnostic. */
NB_EXPORT nb_status nb_status_from_errno(int captured_errno, nb_err *err);

/*
 * Table lookup is a pure function: no global state, no allocation, no locking
 * and no registration order. A NULL/empty table or a miss returns NULL.
 */
NB_EXPORT const nb_err_desc *nb_err_lookup(const nb_err_desc *table, size_t count,
                                           nb_err_code code);

/*
 * Human-readable text for a code: the matching description's message (or name
 * when the message is empty), else the generic nb_status_str() text. User
 * codes without a description stringify as "user error".
 */
struct nb_log;
/* Returns code unchanged and emits a debug record through an explicit logger.
 * Captures no global logger and preserves errno. */
NB_EXPORT nb_err_code nb_err_trace(struct nb_log *log, nb_err_code code, const nb_err_desc *table,
                                   size_t count, const char *file, int line);
NB_EXPORT const char *nb_err_name(const nb_err_desc *table, size_t count, nb_err_code code);
NB_EXPORT const char *nb_err_str(const nb_err_desc *table, size_t count,
                                 nb_err_code code);

#ifdef __cplusplus
}
#endif

#endif /* NB_ERR_H */
