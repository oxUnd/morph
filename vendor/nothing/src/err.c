#include "nb/err.h"
#include "nb/log.h"

#include <stdio.h>
#include <errno.h>
#include <string.h>

void nb_err_clear(nb_err *err)
{
    if (!err) {
        return;
    }
    err->code = NB_OK;
    err->os_errno = 0;
    err->msg[0] = '\0';
}

void nb_err_setv(nb_err *err, nb_err_code code, const char *fmt, va_list ap)
{
    if (!err) {
        return;
    }
    err->code = code;
    err->os_errno = 0;
    if (fmt && fmt[0] != '\0') {
        vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
    } else {
        snprintf(err->msg, sizeof(err->msg), "%s",
                 nb_status_str((nb_status)code));
    }
}

void nb_err_set(nb_err *err, nb_err_code code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    nb_err_setv(err, code, fmt, ap);
    va_end(ap);
}

void nb_err_set_os(nb_err *err, nb_err_code code, int os_errno,
                   const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    nb_err_setv(err, code, fmt, ap);
    va_end(ap);
    if (err) {
        err->os_errno = os_errno;
    }
}

const char *nb_status_str(nb_status status)
{
    if (NB_ERR_IS_USER(status)) {
        return "user error";
    }
    switch (status) {
    case NB_OK:              return "ok";
    case NB_ERR_INVAL:       return "invalid argument";
    case NB_ERR_NOMEM:       return "out of memory";
    case NB_ERR_IO:          return "I/O error";
    case NB_ERR_NOT_FOUND:   return "not found";
    case NB_ERR_EXISTS:      return "already exists";
    case NB_ERR_RANGE:       return "out of range";
    case NB_ERR_TIMEOUT:     return "timeout";
    case NB_ERR_AGAIN:       return "try again";
    case NB_ERR_OS:          return "operating system error";
    case NB_ERR_PARSE:       return "parse error";
    case NB_ERR_STATE:       return "invalid state";
    case NB_ERR_UNSUPPORTED: return "unsupported";
    case NB_ERR_PERMISSION:
        return "permission denied";
    case NB_ERR_CANCELLED:
        return "cancelled";
    case NB_ERR_INTERRUPTED:
        return "interrupted";
    case NB_ERR_TOO_BIG:
        return "input too large";
    case NB_ERR_BAD_HANDLE:
        return "invalid handle";
    case NB_ERR_BUSY:
        return "resource busy";
    case NB_ERR_NO_CHILD:
        return "no child process";
    case NB_ERR_CONNECTION_REFUSED:
        return "connection refused";
    case NB_ERR_CONNECTION_RESET:
        return "connection reset";
    case NB_ERR_NO_SPACE:
        return "no space available";
    case NB_ERR_ENCODING:
        return "invalid encoding";
    case NB_ERR_IN_PROGRESS:
        return "operation in progress";
    case NB_ERR_IS_DIR:
        return "is a directory";
    case NB_ERR_LOOP:
        return "too many symbolic links";
    case NB_ERR_RESOURCE:
        return "resource limit reached";
    case NB_ERR_NAME_TOO_LONG:
        return "name too long";
    case NB_ERR_NETWORK:
        return "network unreachable";
    case NB_ERR_NO_DATA:
        return "no data";
    case NB_ERR_NOT_DIR:
        return "not a directory";
    case NB_ERR_NOT_TERMINAL:
        return "not a terminal";
    case NB_ERR_BROKEN_PIPE:
        return "broken pipe";
    case NB_ERR_PROTOCOL:
        return "protocol error";
    case NB_ERR_CLOSED:
        return "closed";
    case NB_ERR_STALE:
        return "stale object";
    case NB_ERR_CROSS_DEVICE:
        return "cross-device operation";
    case NB_ERR_FORMAT:
        return "invalid format";
    case NB_ERR_NOT_PERMITTED:
        return "operation not permitted";
    default:                 return "unknown error";
    }
}

const nb_err_desc *nb_err_lookup(const nb_err_desc *table, size_t count,
                                 nb_err_code code)
{
    if (!table) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        if (table[i].code == code) {
            return &table[i];
        }
    }
    return NULL;
}

const char *nb_err_str(const nb_err_desc *table, size_t count,
                       nb_err_code code)
{
    const nb_err_desc *desc = nb_err_lookup(table, count, code);
    if (desc) {
        if (desc->message && desc->message[0] != '\0') {
            return desc->message;
        }
        if (desc->name && desc->name[0] != '\0') {
            return desc->name;
        }
    }
    return nb_status_str((nb_status)code);
}

nb_status nb_status_from_errno(int captured_errno, nb_err *err)
{
    nb_status status = NB_ERR_OS;
    switch (captured_errno) {
    case 0:
        status = NB_ERR_IO;
        break;
#ifdef EINVAL
    case EINVAL:
        status = NB_ERR_INVAL;
        break;
#endif
#ifdef ENOMEM
    case ENOMEM:
        status = NB_ERR_NOMEM;
        break;
#endif
#ifdef EIO
    case EIO:
        status = NB_ERR_IO;
        break;
#endif
#ifdef ENOENT
    case ENOENT:
        status = NB_ERR_NOT_FOUND;
        break;
#endif
#ifdef EEXIST
    case EEXIST:
        status = NB_ERR_EXISTS;
        break;
#endif
#ifdef ERANGE
    case ERANGE:
        status = NB_ERR_RANGE;
        break;
#endif
#ifdef EOVERFLOW
    case EOVERFLOW:
        status = NB_ERR_RANGE;
        break;
#endif
#ifdef ETIMEDOUT
    case ETIMEDOUT:
        status = NB_ERR_TIMEOUT;
        break;
#endif
#ifdef EAGAIN
    case EAGAIN:
        status = NB_ERR_AGAIN;
        break;
#endif
#ifdef ENOSYS
    case ENOSYS:
        status = NB_ERR_UNSUPPORTED;
        break;
#endif
#ifdef ENOTSUP
    case ENOTSUP:
        status = NB_ERR_UNSUPPORTED;
        break;
#endif
#ifdef EPROTONOSUPPORT
    case EPROTONOSUPPORT:
        status = NB_ERR_UNSUPPORTED;
        break;
#endif
#ifdef EACCES
    case EACCES:
        status = NB_ERR_PERMISSION;
        break;
#endif
#ifdef EPERM
    case EPERM:
        status = NB_ERR_NOT_PERMITTED;
        break;
#endif
#ifdef ECANCELED
    case ECANCELED:
        status = NB_ERR_CANCELLED;
        break;
#endif
#ifdef EINTR
    case EINTR:
        status = NB_ERR_INTERRUPTED;
        break;
#endif
#ifdef E2BIG
    case E2BIG:
        status = NB_ERR_TOO_BIG;
        break;
#endif
#ifdef EFBIG
    case EFBIG:
        status = NB_ERR_TOO_BIG;
        break;
#endif
#ifdef EBADF
    case EBADF:
        status = NB_ERR_BAD_HANDLE;
        break;
#endif
#ifdef EBUSY
    case EBUSY:
        status = NB_ERR_BUSY;
        break;
#endif
#ifdef EALREADY
    case EALREADY:
        status = NB_ERR_STATE;
        break;
#endif
#ifdef ECHILD
    case ECHILD:
        status = NB_ERR_NO_CHILD;
        break;
#endif
#ifdef ECONNREFUSED
    case ECONNREFUSED:
        status = NB_ERR_CONNECTION_REFUSED;
        break;
#endif
#ifdef ECONNRESET
    case ECONNRESET:
        status = NB_ERR_CONNECTION_RESET;
        break;
#endif
#ifdef EDQUOT
    case EDQUOT:
        status = NB_ERR_NO_SPACE;
        break;
#endif
#ifdef ENOSPC
    case ENOSPC:
        status = NB_ERR_NO_SPACE;
        break;
#endif
#ifdef EILSEQ
    case EILSEQ:
        status = NB_ERR_ENCODING;
        break;
#endif
#ifdef EINPROGRESS
    case EINPROGRESS:
        status = NB_ERR_IN_PROGRESS;
        break;
#endif
#ifdef EISDIR
    case EISDIR:
        status = NB_ERR_IS_DIR;
        break;
#endif
#ifdef ELOOP
    case ELOOP:
        status = NB_ERR_LOOP;
        break;
#endif
#ifdef EMFILE
    case EMFILE:
        status = NB_ERR_RESOURCE;
        break;
#endif
#ifdef ENAMETOOLONG
    case ENAMETOOLONG:
        status = NB_ERR_NAME_TOO_LONG;
        break;
#endif
#ifdef ENETUNREACH
    case ENETUNREACH:
        status = NB_ERR_NETWORK;
        break;
#endif
#ifdef ENODATA
    case ENODATA:
        status = NB_ERR_NO_DATA;
        break;
#endif
#ifdef ENOTDIR
    case ENOTDIR:
        status = NB_ERR_NOT_DIR;
        break;
#endif
#ifdef ENOTTY
    case ENOTTY:
        status = NB_ERR_NOT_TERMINAL;
        break;
#endif
#ifdef EPIPE
    case EPIPE:
        status = NB_ERR_BROKEN_PIPE;
        break;
#endif
#ifdef EPROTO
    case EPROTO:
        status = NB_ERR_PROTOCOL;
        break;
#endif
#ifdef ESHUTDOWN
    case ESHUTDOWN:
        status = NB_ERR_CLOSED;
        break;
#endif
#ifdef ESTALE
    case ESTALE:
        status = NB_ERR_STALE;
        break;
#endif
#ifdef EXDEV
    case EXDEV:
        status = NB_ERR_CROSS_DEVICE;
        break;
#endif
    default:
        break;
    }
    if (err) {
        nb_err_set_os(err, status, captured_errno, "%s",
                      captured_errno ? strerror(captured_errno) : "I/O error");
    }
    return status;
}

nb_err_code nb_err_trace(nb_log *log, nb_err_code code, const nb_err_desc *table, size_t count,
                         const char *file, int line)
{
    int saved_errno = errno;
    if (code < 0) {
        nb_log_write(log, NB_LOG_DEBUG, "Error trace: %s (%s:%d)", nb_err_str(table, count, code),
                     file, line);
    }
    errno = saved_errno;
    return code;
}

const char *nb_err_name(const nb_err_desc *table, size_t count, nb_err_code code)
{
    const nb_err_desc *desc = nb_err_lookup(table, count, code);
    if (desc && desc->name) {
        return desc->name;
    }
    switch (code) {
    case NB_ERR_AGAIN:
        return "NB_ERR_AGAIN";
    case NB_ERR_BAD_HANDLE:
        return "NB_ERR_BAD_HANDLE";
    case NB_ERR_BROKEN_PIPE:
        return "NB_ERR_BROKEN_PIPE";
    case NB_ERR_BUSY:
        return "NB_ERR_BUSY";
    case NB_ERR_CANCELLED:
        return "NB_ERR_CANCELLED";
    case NB_ERR_CLOSED:
        return "NB_ERR_CLOSED";
    case NB_ERR_CONNECTION_REFUSED:
        return "NB_ERR_CONNECTION_REFUSED";
    case NB_ERR_CONNECTION_RESET:
        return "NB_ERR_CONNECTION_RESET";
    case NB_ERR_CROSS_DEVICE:
        return "NB_ERR_CROSS_DEVICE";
    case NB_ERR_FORMAT:
        return "NB_ERR_FORMAT";
    case NB_ERR_NOT_PERMITTED:
        return "NB_ERR_NOT_PERMITTED";
    case NB_ERR_ENCODING:
        return "NB_ERR_ENCODING";
    case NB_ERR_EXISTS:
        return "NB_ERR_EXISTS";
    case NB_ERR_INTERRUPTED:
        return "NB_ERR_INTERRUPTED";
    case NB_ERR_INVAL:
        return "NB_ERR_INVAL";
    case NB_ERR_IN_PROGRESS:
        return "NB_ERR_IN_PROGRESS";
    case NB_ERR_IO:
        return "NB_ERR_IO";
    case NB_ERR_IS_DIR:
        return "NB_ERR_IS_DIR";
    case NB_ERR_LOOP:
        return "NB_ERR_LOOP";
    case NB_ERR_NAME_TOO_LONG:
        return "NB_ERR_NAME_TOO_LONG";
    case NB_ERR_NETWORK:
        return "NB_ERR_NETWORK";
    case NB_ERR_NOMEM:
        return "NB_ERR_NOMEM";
    case NB_ERR_NOT_DIR:
        return "NB_ERR_NOT_DIR";
    case NB_ERR_NOT_FOUND:
        return "NB_ERR_NOT_FOUND";
    case NB_ERR_NOT_TERMINAL:
        return "NB_ERR_NOT_TERMINAL";
    case NB_ERR_NO_CHILD:
        return "NB_ERR_NO_CHILD";
    case NB_ERR_NO_DATA:
        return "NB_ERR_NO_DATA";
    case NB_ERR_NO_SPACE:
        return "NB_ERR_NO_SPACE";
    case NB_OK:
        return "NB_OK";
    case NB_ERR_OS:
        return "NB_ERR_OS";
    case NB_ERR_PARSE:
        return "NB_ERR_PARSE";
    case NB_ERR_PERMISSION:
        return "NB_ERR_PERMISSION";
    case NB_ERR_PROTOCOL:
        return "NB_ERR_PROTOCOL";
    case NB_ERR_RANGE:
        return "NB_ERR_RANGE";
    case NB_ERR_RESOURCE:
        return "NB_ERR_RESOURCE";
    case NB_ERR_STALE:
        return "NB_ERR_STALE";
    case NB_ERR_STATE:
        return "NB_ERR_STATE";
    case NB_ERR_TIMEOUT:
        return "NB_ERR_TIMEOUT";
    case NB_ERR_TOO_BIG:
        return "NB_ERR_TOO_BIG";
    case NB_ERR_UNSUPPORTED:
        return "NB_ERR_UNSUPPORTED";
    default:
        return "UNKNOWN";
    }
}
