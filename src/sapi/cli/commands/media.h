#ifndef MORPH_CLI_COMMANDS_MEDIA_H
#define MORPH_CLI_COMMANDS_MEDIA_H

#include "sapi/cli/commands/registry.h"

struct cli_application;

int cli_media_attach_image(struct cli_application *ctx, const char *path);
int cli_media_handle_path(struct cli_application *ctx, const char *input, int *handled);

struct cli_command_group cli_media_commands(void);

#endif
