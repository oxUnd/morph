#ifndef MORPH_CLI_PAGES_SETUP_CONTROLS_H
#define MORPH_CLI_PAGES_SETUP_CONTROLS_H

struct cli_application;
struct cli_setup_state;
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

int cli_setup_controls_available(FILE *input, FILE *output);
/* Terminal receipts share the wizard margin; plain output keeps its format. */
int cli_setup_print(FILE *output, const char *format, ...);
int cli_setup_controls_collect(struct cli_application *ctx, const char *path, FILE *input,
			       FILE *output, struct cli_setup_state *state);

#ifdef __cplusplus
}
#endif

#endif
