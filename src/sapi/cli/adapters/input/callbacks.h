#ifndef MORPH_CLI_ADAPTERS_INPUT_CALLBACKS_H
#define MORPH_CLI_ADAPTERS_INPUT_CALLBACKS_H

#include <stdio.h>
#include <stdint.h>

struct cli_application;
struct cli_callback;

/* Instance-owned closures adapt APIs that have no userdata parameter. */
int (*cli_callback_getc(struct cli_application *ctx,
			int (*callback)(struct cli_application *, FILE *)))(FILE *);
void cli_callbacks_cleanup(struct cli_application *ctx);

#endif
