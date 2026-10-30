#ifndef MORPH_CLI_SETUP_H
#define MORPH_CLI_SETUP_H

struct cli_context;
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create a new config. Returns 1 when credentials still need to be set. */
int cli_setup(struct cli_context *ctx, const char *path, FILE *input, FILE *output);
/* Reconfigure an existing regular config, backing it up after confirmation. */
int cli_setup_reconfigure(struct cli_context *ctx, const char *path, FILE *input, FILE *output);
int cli_setup_command(struct cli_context *ctx, int argc, char **argv, const char *path);
/* Existing file: 0. Completed setup: 1 (caller must exit). Failure: negative. */
int cli_setup_if_missing(struct cli_context *ctx, const char *path, int interactive);

#ifdef __cplusplus
}
#endif
#endif
