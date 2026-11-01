#ifndef MORPH_CLI_ADAPTERS_INPUT_CLIPBOARD_H
#define MORPH_CLI_ADAPTERS_INPUT_CLIPBOARD_H

struct cli_application;

int cli_clipboard_save_image(struct cli_application *ctx, char **out_path);

#endif
