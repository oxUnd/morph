#ifndef MORPH_CLI_HISTORY_UI_H
#define MORPH_CLI_HISTORY_UI_H

#include "sapi/cli/cli.h"

int cli_history_view(struct cli_context *ctx, cJSON *report);
void cli_history_print(const cJSON *report, int limit, int all);
int cli_history_entry_print(struct cli_context *ctx, const cJSON *report,
	int64_t sequence, int stored);
int cli_history_request_print(struct cli_context *ctx);

#endif
