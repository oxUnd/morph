#ifndef MORPH_CLI_SERVICES_MANAGEMENT_H
#define MORPH_CLI_SERVICES_MANAGEMENT_H

struct morph_log;
#include "util/buf.h"
#include <stddef.h>

/* Management commands run without opening a conversational runtime. */
int cli_management_run(const char *dir, char *const argv[], morph_buf_t *output);
int cli_management_atomic_write(const char *path, const char *data, size_t len);
int cli_management_lock(const char *path);
int cli_management_name_valid(const char *name);
int cli_management_copy_arg(char *dst, size_t cap, const char *src);
int cli_management_error(int rc, const char *context);
void cli_management_reset_getopt(void);

#endif
