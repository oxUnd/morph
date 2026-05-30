#include "log.h"
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#define MAX_LOG_FILE_SIZE (5 * 1024 * 1024)
#define MAX_LOG_FILES 3

static struct {
	enum log_level level;
	FILE *file;
	char path[PATH_MAX];
} log_state = {
	.level = LOG_INFO,
	.file = NULL,
	.path = {0},
};

static const char *level_strings[] = {
	[LOG_DEBUG] = "DEBUG",
	[LOG_INFO] = "INFO",
	[LOG_WARN] = "WARN",
	[LOG_ERROR] = "ERROR",
};

void log_init(const char *file, enum log_level level)
{
	log_state.level = level;
	if (file) {
		strncpy(log_state.path, file, sizeof(log_state.path) - 1);
		log_state.path[sizeof(log_state.path) - 1] = '\0';
		log_state.file = fopen(file, "a");
	}
}

void log_set_level(enum log_level level)
{
	log_state.level = level;
}

static void rotate_log(void)
{
	if (!log_state.file)
		return;
	long pos = ftell(log_state.file);
	if (pos < MAX_LOG_FILE_SIZE)
		return;

	char old_path[PATH_MAX + 4];
	snprintf(old_path, sizeof(old_path), "%s.old", log_state.path);
	fclose(log_state.file);
	rename(log_state.path, old_path);

	log_state.file = fopen(log_state.path, "a");
	if (!log_state.file) {
		log_state.file = fopen(old_path, "a");
		return;
	}

	char backup[520];
	snprintf(backup, sizeof(backup), "%s.1", log_state.path);
	for (int i = MAX_LOG_FILES; i > 1; i--) {
		char from[520], to[520];
		snprintf(from, sizeof(from), "%s.%d", log_state.path, i - 1);
		snprintf(to, sizeof(to), "%s.%d", log_state.path, i);
		rename(from, to);
	}
	rename(old_path, backup);
}

void log_write(enum log_level level, const char *fmt, ...)
{
	if (level < log_state.level)
		return;

	time_t now = time(NULL);
	struct tm *t = localtime(&now);
	char ts[32];
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);

	va_list ap;
	va_start(ap, fmt);

	if (log_state.file) {
		fprintf(log_state.file, "[%s] [%s] ", ts, level_strings[level]);
		vfprintf(log_state.file, fmt, ap);
		fprintf(log_state.file, "\n");
		fflush(log_state.file);
	}

	va_end(ap);
	rotate_log();
}

void log_shutdown(void)
{
	if (log_state.file) {
		fclose(log_state.file);
		log_state.file = NULL;
	}
}