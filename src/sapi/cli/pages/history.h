#ifndef MORPH_CLI_PAGES_HISTORY_H
#define MORPH_CLI_PAGES_HISTORY_H

struct cli_application;
#include "sapi/cli/application.h"

int cli_history_view(struct cli_application *ctx, struct json *report);
void cli_history_print(struct cli_application *ctx, const struct json *report, int limit, int all);
int cli_history_entry_print(struct cli_application *ctx, const struct json *report,
			    int64_t sequence, int stored);
int cli_history_request_print(struct cli_application *ctx);

#endif
