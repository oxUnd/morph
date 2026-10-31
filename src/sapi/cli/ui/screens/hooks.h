#ifndef MORPH_CLI_UI_SCREENS_HOOKS_H
#define MORPH_CLI_UI_SCREENS_HOOKS_H

struct cli_context;
#include "sapi/cli/cli.h"

int cli_hooks_ui(struct cli_context *ctx, cJSON **rows, const char *review_id);

int cli_hooks_review_all(struct cli_context *ctx, cJSON **rows);
int cli_hooks_startup(struct cli_context *ctx);

#endif
