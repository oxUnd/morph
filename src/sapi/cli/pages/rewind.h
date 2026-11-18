#ifndef MORPH_CLI_PAGES_REWIND_H
#define MORPH_CLI_PAGES_REWIND_H

#include "runtime/checkpoint.h"

struct cli_application;
typedef int (*cli_rewind_apply_fn)(struct cli_application *, int64_t, enum runtime_rewind_mode);

/* Success takes listing and clears its source; failure preserves it.
 * apply is borrowed until the picker closes. */
int cli_rewind_show_take(struct cli_application *ctx, cJSON **listing, cli_rewind_apply_fn apply);

#endif
