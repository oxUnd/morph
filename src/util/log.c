#include "log.h"
#include "file.h"
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

	char old_path[PATH_MAX + 16];
	snprintf(old_path, sizeof(old_path), "%s", log_state.path);
	if (file_path_append(old_path, sizeof(old_path), ".old") != 0)
		return;
	fclose(log_state.file);
	rename(log_state.path, old_path);

	log_state.file = fopen(log_state.path, "a");
	if (!log_state.file) {
		log_state.file = fopen(old_path, "a");
		return;
	}

	char backup[PATH_MAX + 16];
	snprintf(backup, sizeof(backup), "%s", log_state.path);
	if (file_path_append(backup, sizeof(backup), ".1") != 0)
		return;
	for (int i = MAX_LOG_FILES; i > 1; i--) {
		char from[PATH_MAX + 16], to[PATH_MAX + 16];
		snprintf(from, sizeof(from), "%s", log_state.path);
		snprintf(to, sizeof(to), "%s", log_state.path);
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
