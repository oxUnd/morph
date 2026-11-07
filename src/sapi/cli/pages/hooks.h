#ifndef MORPH_CLI_PAGES_HOOKS_H
#define MORPH_CLI_PAGES_HOOKS_H

struct cli_application;
#include "sapi/cli/application.h"

int cli_hooks_ui(struct cli_application *ctx, struct json **rows, const char *review_id);

int cli_hooks_review_all(struct cli_application *ctx, struct json **rows);
int cli_hooks_startup(struct cli_application *ctx);

#endif
