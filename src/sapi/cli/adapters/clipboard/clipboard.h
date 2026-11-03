#ifndef MORPH_CLI_ADAPTERS_CLIPBOARD_CLIPBOARD_H
#define MORPH_CLI_ADAPTERS_CLIPBOARD_CLIPBOARD_H

struct cli_application;

int cli_clipboard_save_image(struct cli_application *ctx, char **out_path);

enum cli_clipboard_copy_result {
	CLI_CLIPBOARD_COPIED = 0,
	CLI_CLIPBOARD_REQUESTED = 1,
};

/* Native success is confirmed. Terminal-only OSC 52 delivery is unconfirmed. */
int cli_clipboard_copy_text(struct cli_application *ctx, const char *text);

#endif
