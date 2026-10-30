#include "log.h"
#include "file.h"
#include "error.h"
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#define MAX_LOG_FILE_SIZE (5 * 1024 * 1024)
#define MAX_LOG_FILES 3

static const char *const level_strings[] = {
	[LOG_DEBUG] = "DEBUG",
	[LOG_INFO] = "INFO",
	[LOG_WARN] = "WARN",
	[LOG_ERROR] = "ERROR",
};

int log_init(struct morph_log *logger, const char *file, enum log_level level)
{
	int error;

	if (!logger || level < LOG_DEBUG || level > LOG_ERROR)
		return -EINVAL;
	memset(logger, 0, sizeof(*logger));
	if (file && strlen(file) >= sizeof(logger->path))
		return -ENAMETOOLONG;
	error = pthread_mutex_init(&logger->lock, NULL);
	if (error)
		return -error;
	logger->level = level;
	if (file) {
		memcpy(logger->path, file, strlen(file) + 1);
		logger->file = fopen(file, "a");
		if (logger->file)
			(void)fcntl(fileno(logger->file), F_SETFD, FD_CLOEXEC);
		if (!logger->file) {
			error = errno;
			pthread_mutex_destroy(&logger->lock);
			return -error;
		}
	}
	logger->initialized = 1;
	return 0;
}

void log_set_level(struct morph_log *logger, enum log_level level)
{
	if (!logger || !logger->initialized)
		return;
	pthread_mutex_lock(&logger->lock);
	logger->level = level;
	pthread_mutex_unlock(&logger->lock);
}

static void rotate_log(struct morph_log *logger)
{
	if (!logger->file)
		return;
	long pos = ftell(logger->file);
	if (pos < MAX_LOG_FILE_SIZE)
		return;

	char old_path[PATH_MAX + 16];
	snprintf(old_path, sizeof(old_path), "%s", logger->path);
	if (file_path_append(old_path, sizeof(old_path), ".old") != 0)
		return;
	fclose(logger->file);
	rename(logger->path, old_path);

	logger->file = fopen(logger->path, "a");
	if (logger->file)
		(void)fcntl(fileno(logger->file), F_SETFD, FD_CLOEXEC);
	if (!logger->file) {
		logger->file = fopen(old_path, "a");
		if (logger->file)
			(void)fcntl(fileno(logger->file), F_SETFD, FD_CLOEXEC);
		return;
	}

	char backup[PATH_MAX + 16];
	snprintf(backup, sizeof(backup), "%s", logger->path);
	if (file_path_append(backup, sizeof(backup), ".1") != 0)
		return;
	for (int i = MAX_LOG_FILES; i > 1; i--) {
		char from[PATH_MAX + 16], to[PATH_MAX + 16];
		snprintf(from, sizeof(from), "%s", logger->path);
		snprintf(to, sizeof(to), "%s", logger->path);
		char suffix[16];
		snprintf(suffix, sizeof(suffix), ".%d", i - 1);
		if (file_path_append(from, sizeof(from), suffix) != 0)
			continue;
		snprintf(suffix, sizeof(suffix), ".%d", i);
		if (file_path_append(to, sizeof(to), suffix) != 0)
			continue;
		rename(from, to);
	}
	rename(old_path, backup);
}

static void log_vwrite(struct morph_log *logger, enum log_level level,
		       const char *fmt, va_list args)
{
	struct tm local;
	char timestamp[32];
	time_t now;

	if (!logger || !logger->initialized || !fmt || level < LOG_DEBUG || level > LOG_ERROR)
		return;
	pthread_mutex_lock(&logger->lock);
	if (level >= logger->level && logger->file) {
		now = time(NULL);
		localtime_r(&now, &local);
		strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);
		fprintf(logger->file, "[%s] [%s] ", timestamp, level_strings[level]);
		vfprintf(logger->file, fmt, args);
		fputc('\n', logger->file);
		fflush(logger->file);
		rotate_log(logger);
	}
	pthread_mutex_unlock(&logger->lock);
}

void log_write(struct morph_log *logger, enum log_level level, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	log_vwrite(logger, level, fmt, args);
	va_end(args);
}

void log_shutdown(struct morph_log *logger)
{
	if (!logger || !logger->initialized)
		return;
	pthread_mutex_lock(&logger->lock);
	if (logger->file)
		fclose(logger->file);
	logger->file = NULL;
	logger->initialized = 0;
	pthread_mutex_unlock(&logger->lock);
	pthread_mutex_destroy(&logger->lock);
}

void log_diagnostic(struct morph_log *logger, const char *fmt, ...)
{
	va_list args;
	int saved_errno = errno;

	va_start(args, fmt);
	log_vwrite(logger, LOG_DEBUG, fmt, args);
	va_end(args);
	errno = saved_errno;
}
