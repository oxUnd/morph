#ifndef MORPH_CLI_MANAGE_H
#define MORPH_CLI_MANAGE_H

#include "util/buf.h"
#include <stddef.h>

/* Management commands run without opening a conversational runtime. */
int cli_manage_mcp(int argc, char **argv, const char *config_path);
int cli_manage_skills(int argc, char **argv, const char *config_path);
int cli_manage_run(const char *dir, char *const argv[], morph_buf_t *output);
int cli_manage_atomic_write(const char *path, const char *data, size_t len);
int cli_manage_lock(const char *path);
int cli_manage_name_valid(const char *name);
int cli_manage_copy_arg(char *dst, size_t cap, const char *src);
int cli_manage_error(int rc, const char *context);
void cli_manage_reset_getopt(void);

#endif
