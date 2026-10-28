#ifndef LOG_H
#define LOG_H

#include <limits.h>
#include <pthread.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum log_level {
	LOG_DEBUG = 0,
	LOG_INFO,
	LOG_WARN,
	LOG_ERROR,
};

/* Initialize once; the owner stops writers before shutting this instance down. */
struct morph_log {
	enum log_level level;
	FILE *file;
	char path[PATH_MAX];
	pthread_mutex_t lock;
	int initialized;
};

int log_init(struct morph_log *logger, const char *file, enum log_level level);
void log_set_level(struct morph_log *logger, enum log_level level);
void log_write(struct morph_log *logger, enum log_level level, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
void log_shutdown(struct morph_log *logger);
void log_diagnostic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define log_dbg(logger, fmt, ...) log_write(logger, LOG_DEBUG, fmt, ##__VA_ARGS__)
#define log_info(logger, fmt, ...) log_write(logger, LOG_INFO, fmt, ##__VA_ARGS__)
#define log_warn(logger, fmt, ...) log_write(logger, LOG_WARN, fmt, ##__VA_ARGS__)
#define log_err(logger, fmt, ...) log_write(logger, LOG_ERROR, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
