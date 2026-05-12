#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#define MAX_LOG_FILE_SIZE (5 * 1024 * 1024)
#define MAX_LOG_FILES 3

static struct {
	enum log_level level;
	FILE *file;
	char path[512];
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
	fclose(log_state.file);
	for (int i = MAX_LOG_FILES - 1; i > 0; i--) {
		char old_name[520], new_name[520];
		snprintf(old_name, sizeof(old_name), "%s.%d", log_state.path, i);
		snprintf(new_name, sizeof(new_name), "%s.%d", log_state.path, i + 1);
		rename(old_name, new_name);
	}
	char backup[520];
	snprintf(backup, sizeof(backup), "%s.1", log_state.path);
	rename(log_state.path, backup);
	log_state.file = fopen(log_state.path, "a");
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

	FILE *targets[2] = {stderr, log_state.file};
	for (int i = 0; i < 2; i++) {
		if (!targets[i])
			continue;
		fprintf(targets[i], "[%s] [%s] ", ts, level_strings[level]);
		vfprintf(targets[i], fmt, ap);
		fprintf(targets[i], "\n");
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