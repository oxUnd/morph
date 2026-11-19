#ifndef NB_LOG_H
#define NB_LOG_H

#include "nb/base.h"

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nb_log_level {
    NB_LOG_TRACE = 0,
    NB_LOG_DEBUG,
    NB_LOG_INFO,
    NB_LOG_WARN,
    NB_LOG_ERROR,
    NB_LOG_FATAL,
    NB_LOG_OFF
} nb_log_level;

/*
 * A sink receives one complete record, including the trailing newline.
 * Sinks are called with the logger's mutex held and must not call back into
 * the same logger.
 */
typedef void (*nb_log_sink)(void *ctx, nb_log_level level,
                            const char *record, size_t len);

typedef struct nb_log nb_log;

NB_EXPORT nb_log *nb_log_new(nb_log_level min_level, nb_log_sink sink, void *ctx);
/* Owns its rotated file sink until nb_log_free(). NULL path returns NULL,
 * allowing an application to disable logging without using the default sink. */
NB_EXPORT nb_log *nb_log_new_file(nb_log_level min_level, const char *path, size_t max_bytes,
                                  unsigned max_files);
NB_EXPORT void nb_log_free(nb_log *log);
NB_EXPORT void nb_log_set_level(nb_log *log, nb_log_level min_level);
NB_EXPORT nb_log_level nb_log_get_level(const nb_log *log);

/* Replaces the sink and its context. A NULL sink restores the default stderr
 * sink. Safe to call while other threads are logging. */
NB_EXPORT void nb_log_set_sink(nb_log *log, nb_log_sink sink, void *ctx);

NB_EXPORT void nb_log_write(nb_log *log, nb_log_level level,
                            const char *fmt, ...) NB_PRINTF(3, 4);
NB_EXPORT void nb_log_vwrite(nb_log *log, nb_log_level level,
                             const char *fmt, va_list ap);

NB_EXPORT const char *nb_log_level_str(nb_log_level level);
NB_EXPORT nb_log_level nb_log_level_from_str(const char *name, bool *ok);

typedef struct nb_log_file nb_log_file;

/* Opens path for append and returns a sink context with size-based rotation.
 * max_bytes == 0 disables rotation. Otherwise max_files rotated backups are
 * kept as path.1 .. path.max_files; max_files must be at least 1. The parent
 * directory must already exist. Returns NULL for invalid arguments or when
 * the file cannot be opened. */
NB_EXPORT nb_log_file *nb_log_file_open(const char *path, size_t max_bytes, unsigned max_files);
NB_EXPORT void nb_log_file_close(nb_log_file *file);
/* nb_log_sink callback. Pass the returned handle as ctx. */
NB_EXPORT void nb_log_file_sink(void *ctx, nb_log_level level, const char *record, size_t len);

/* Process-wide logger writing to stderr. It is created on first use and is
 * safe to call from multiple threads. */
NB_EXPORT nb_log *nb_log_default(void);

/* Replaces the process-wide logger and returns the previous one. The caller
 * keeps ownership of log and must keep it alive while it is installed. */
NB_EXPORT nb_log *nb_log_set_default(nb_log *log);

#ifdef __cplusplus
}
#endif

#endif /* NB_LOG_H */
