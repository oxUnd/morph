#ifndef MORPH_CLI_HOOKS_UI_H
#define MORPH_CLI_HOOKS_UI_H

#include "sapi/cli/cli.h"

int cli_hooks_ui(struct cli_context *ctx, cJSON **rows, const char *review_id);

int cli_hooks_review_all(struct cli_context *ctx, cJSON **rows);
int cli_hooks_startup(struct cli_context *ctx);

#endif
