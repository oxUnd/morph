#ifndef MORPH_CLI_UI_INPUT_CLIPBOARD_H
#define MORPH_CLI_UI_INPUT_CLIPBOARD_H

struct cli_context;

int cli_clipboard_save_image(struct cli_context *ctx, char **out_path);

#endif
