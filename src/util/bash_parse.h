#ifndef MORPH_BASH_PARSE_H
#define MORPH_BASH_PARSE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <stddef.h>
#include "array.h"

#define BASH_PARSE_COMMAND_NAME_MAX 256

struct bash_parse_command {
	char program[PATH_MAX];
	char name[BASH_PARSE_COMMAND_NAME_MAX];
	size_t start_byte;
	size_t end_byte;
};

struct bash_parse_result {
	morph_array_t commands;
	int is_compound;
	int has_error;
};

int bash_parse_analyze(const char *source, struct bash_parse_result *result);
void bash_parse_result_cleanup(struct bash_parse_result *result);
const struct bash_parse_command *bash_parse_first_command(
	const struct bash_parse_result *result);
int bash_parse_command_name(const char *source, char *out, size_t out_size);
int bash_parse_command_program(const char *source, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
