#ifndef MORPH_CLI_SERVICES_MEMORY_H
#define MORPH_CLI_SERVICES_MEMORY_H

#include "cJSON.h"

struct cli_application;

int cli_memory_request(struct cli_application *ctx, const char *method,
	const cJSON *params, cJSON **out);
char *cli_memory_text_copy(struct cli_application *ctx, const char *method,
	const cJSON *params);

#endif
