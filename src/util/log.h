#ifndef LOG_H
#define LOG_H

#ifdef __cplusplus
extern "C" {
#endif

enum log_level {
	LOG_DEBUG = 0,
	LOG_INFO,
	LOG_WARN,
	LOG_ERROR,
};

void log_init(const char *file, enum log_level level);
void log_set_level(enum log_level level);
void log_write(enum log_level level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
void log_shutdown(void);

#define log_dbg(fmt, ...) log_write(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define log_info(fmt, ...) log_write(LOG_INFO, fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...) log_write(LOG_WARN, fmt, ##__VA_ARGS__)
#define log_err(fmt, ...) log_write(LOG_ERROR, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
