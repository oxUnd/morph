#include "nb/log.h"

#include "nb/mem.h"
#include "nb/buf.h"
#include <errno.h>
#include "nb/str.h"
#include "nb/thread.h"
#include "nb/time.h"
#include "nb/utf8.h"
#include "platform/nb_platform.h"

#include <stdio.h>
#include <string.h>

struct nb_log {
    _Atomic int level;
    nb_log_sink sink;
    void *ctx;
    nb_mutex *mutex;
    nb_log_file *owned_file;
};

static void default_sink(void *ctx, nb_log_level level,
                         const char *record, size_t len)
{
    (void)ctx;
    (void)level;
    fwrite(record, 1, len, stderr);
    fflush(stderr);
}

nb_log *nb_log_new(nb_log_level min_level, nb_log_sink sink, void *ctx)
{
    if (!sink) {
        sink = default_sink;
    }
    nb_log *log = nb_try_alloc(sizeof(*log));
    if (!log) {
        return NULL;
    }
    log->mutex = nb_mutex_new(0);
    if (!log->mutex) {
        nb_free(log, sizeof(*log));
        return NULL;
    }
    atomic_store_explicit(&log->level, (int)min_level, memory_order_relaxed);
    log->sink = sink;
    log->ctx = ctx;
    log->owned_file = NULL;
    return log;
}

nb_log *nb_log_new_file(nb_log_level min_level, const char *path, size_t max_bytes,
                        unsigned max_files)
{
    if (!path) {
        return NULL;
    }
    nb_log_file *file = nb_log_file_open(path, max_bytes, max_files);
    if (!file) {
        return NULL;
    }
    nb_log *log = nb_log_new(min_level, nb_log_file_sink, file);
    if (!log) {
        nb_log_file_close(file);
        return NULL;
    }
    log->owned_file = file;
    return log;
}

void nb_log_free(nb_log *log)
{
    if (!log) {
        return;
    }
    nb_log_file_close(log->owned_file);
    nb_mutex_free(log->mutex);
    nb_free(log, sizeof(*log));
}

void nb_log_set_level(nb_log *log, nb_log_level min_level)
{
    if (log) {
        atomic_store_explicit(&log->level, (int)min_level, memory_order_relaxed);
    }
}

nb_log_level nb_log_get_level(const nb_log *log)
{
    if (!log) {
        return NB_LOG_OFF;
    }
    return (nb_log_level)atomic_load_explicit(&log->level, memory_order_relaxed);
}

void nb_log_set_sink(nb_log *log, nb_log_sink sink, void *ctx)
{
    if (!log) {
        return;
    }
    if (!sink) {
        sink = default_sink;
        ctx = NULL;
    }
    nb_mutex_lock(log->mutex);
    log->sink = sink;
    log->ctx = ctx;
    nb_mutex_unlock(log->mutex);
}

void nb_log_vwrite(nb_log *log, nb_log_level level, const char *fmt, va_list ap)
{
    if (!log || !fmt || level < 0 || level >= NB_LOG_OFF) {
        return;
    }
    nb_log_level min_level =
        (nb_log_level)atomic_load_explicit(&log->level, memory_order_relaxed);
    if (level < min_level) {
        return;
    }

    int saved_errno = errno;
    nb_buf message;
    nb_buf_init(&message);
    if (!nb_buf_vprintf(&message, fmt, ap)) {
        nb_buf_destroy(&message);
        errno = saved_errno;
        return;
    }
    char timestamp[64];
    if (!nb_time_format_rfc3339(nb_time_now_ns(), timestamp, sizeof(timestamp))) {
        snprintf(timestamp, sizeof(timestamp), "-");
    }
    nb_buf record;
    nb_buf_init(&record);
    bool ok = nb_buf_printf(&record, "%s [%s] %s\n", timestamp, nb_log_level_str(level),
                            nb_buf_cstr(&message));
    if (ok) {
        nb_mutex_lock(log->mutex);
        log->sink(log->ctx, level, nb_buf_cstr(&record), nb_buf_len(&record));
        nb_mutex_unlock(log->mutex);
    }
    nb_buf_destroy(&record);
    nb_buf_destroy(&message);
    errno = saved_errno;
}

void nb_log_write(nb_log *log, nb_log_level level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    nb_log_vwrite(log, level, fmt, ap);
    va_end(ap);
}

const char *nb_log_level_str(nb_log_level level)
{
    switch (level) {
    case NB_LOG_TRACE: return "TRACE";
    case NB_LOG_DEBUG: return "DEBUG";
    case NB_LOG_INFO:  return "INFO";
    case NB_LOG_WARN:  return "WARN";
    case NB_LOG_ERROR: return "ERROR";
    case NB_LOG_FATAL: return "FATAL";
    case NB_LOG_OFF:   return "OFF";
    default:           return "?";
    }
}

static bool equals_ignore_case(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

nb_log_level nb_log_level_from_str(const char *name, bool *ok)
{
    static const struct {
        const char *name;
        nb_log_level level;
    } levels[] = {
        { "trace", NB_LOG_TRACE },
        { "debug", NB_LOG_DEBUG },
        { "info", NB_LOG_INFO },
        { "warn", NB_LOG_WARN },
        { "warning", NB_LOG_WARN },
        { "error", NB_LOG_ERROR },
        { "fatal", NB_LOG_FATAL },
        { "off", NB_LOG_OFF },
    };
    if (name) {
        for (size_t i = 0; i < NB_ARRAY_SIZE(levels); i++) {
            if (equals_ignore_case(name, levels[i].name)) {
                if (ok) {
                    *ok = true;
                }
                return levels[i].level;
            }
        }
    }
    if (ok) {
        *ok = false;
    }
    return NB_LOG_INFO;
}

static nb_log *g_default_log = NULL;
static bool g_default_explicit = false;

nb_log *nb_log_default(void)
{
    nb_platform_log_lock();
    if (!g_default_log && !g_default_explicit) {
        g_default_log = nb_log_new(NB_LOG_INFO, default_sink, NULL);
    }
    nb_log *log = g_default_log;
    nb_platform_log_unlock();
    return log;
}

nb_log *nb_log_set_default(nb_log *log)
{
    nb_platform_log_lock();
    g_default_explicit = true;
    nb_log *previous = g_default_log;
    g_default_log = log;
    nb_platform_log_unlock();
    return previous;
}
