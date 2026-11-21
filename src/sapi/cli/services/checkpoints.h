#ifndef MORPH_CLI_SERVICES_CHECKPOINTS_H
#define MORPH_CLI_SERVICES_CHECKPOINTS_H

#include "runtime/checkpoint.h"

struct cli_application;

int cli_checkpoints_list(struct cli_application *ctx, cJSON **out);
int cli_checkpoints_preview(struct cli_application *ctx, int64_t id, cJSON **out);
int cli_checkpoints_restore(struct cli_application *ctx, int64_t id,
	enum runtime_rewind_mode mode, cJSON **out);

#endif
