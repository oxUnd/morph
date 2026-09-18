#ifndef MORPH_COMMAND_ANALYZER_H
#define MORPH_COMMAND_ANALYZER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

enum command_type {
	COMMAND_SHELL = 0,
	COMMAND_ARGV,
};

struct command_segment {
	char *raw;
	char **argv;
	size_t argc;
	int parsed;
};

struct command_analysis {
	struct command_segment *segments;
	size_t count;
	int complex;
};

int command_analyze(const char *command, struct command_analysis *out);
void command_analysis_cleanup(struct command_analysis *analysis);

#ifdef __cplusplus
}
#endif

#endif
