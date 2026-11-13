#ifndef MORPH_CLI_SERVICES_MANAGEMENT_H
#define MORPH_CLI_SERVICES_MANAGEMENT_H

struct nb_log;
#include "nb/buf.h"
#include "nb/cleanup.h"
#include <stddef.h>

/* Management commands run without opening a conversational runtime. */
int cli_management_run(const char *dir, char *const argv[], nb_buf *output);
int cli_management_lock(const char *path);
int cli_management_name_valid(const char *name);
int cli_management_copy_arg(char *dst, size_t cap, const char *src);
int cli_management_error(int rc, const char *context);
void cli_management_reset_getopt(void);

#endif
