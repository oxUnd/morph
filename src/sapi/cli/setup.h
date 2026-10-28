#ifndef MORPH_CLI_SETUP_H
#define MORPH_CLI_SETUP_H

struct cli_context;
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create a new config. Returns 1 when credentials still need to be set. */
int cli_setup(struct cli_context *ctx, const char *path, FILE *input, FILE *output);
/* Prompt only on an interactive terminal; existing files are untouched. */
int cli_setup_if_missing(struct cli_context *ctx, const char *path, int interactive);

#ifdef __cplusplus
}
#endif
#endif
