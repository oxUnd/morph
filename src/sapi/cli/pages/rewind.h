#ifndef MORPH_CLI_PAGES_REWIND_H
#define MORPH_CLI_PAGES_REWIND_H

#include "runtime/checkpoint.h"

struct cli_application;
struct nt_view_controller;
typedef int (*cli_rewind_apply_fn)(struct cli_application *, int64_t, enum runtime_rewind_mode);

int cli_rewind_create_take(cJSON **listing, cli_rewind_apply_fn apply,
			   struct nt_view_controller **out);

#endif
