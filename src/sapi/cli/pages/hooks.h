#ifndef MORPH_CLI_PAGES_HOOKS_H
#define MORPH_CLI_PAGES_HOOKS_H

#include "sapi/cli/application.h"

struct cli_application;

int cli_hooks_ui(struct cli_application *ctx, cJSON **rows, const char *review_id);

int cli_hooks_review_all(struct cli_application *ctx, cJSON **rows);
int cli_hooks_startup(struct cli_application *ctx);

#endif
