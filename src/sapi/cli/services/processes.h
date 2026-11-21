#ifndef MORPH_CLI_SERVICES_PROCESSES_H
#define MORPH_CLI_SERVICES_PROCESSES_H

#include "cJSON.h"

struct cli_application;

int cli_process_list(struct cli_application *ctx, cJSON **out);
int cli_process_read(struct cli_application *ctx, const char *id, cJSON **out);
int cli_process_stop(struct cli_application *ctx, const char *id);

#endif
