#ifndef MORPH_CLI_COMMANDS_MEDIA_H
#define MORPH_CLI_COMMANDS_MEDIA_H

struct cli_context;

int cli_attach_image(struct cli_context *ctx, const char *path);
int cli_handle_media_path(struct cli_context *ctx, const char *input, int *handled);

#endif
