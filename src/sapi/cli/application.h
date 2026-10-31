#ifndef CLI_APPLICATION_H
#define CLI_APPLICATION_H

#include <morph/tui/application.h>

struct cli_context;

/* Takes ownership of data only on success. The completion runs after the
 * panel has left the terminal and may release the complete page model. */
int cli_screen_present(struct cli_context *ctx,
		       const struct morph_tui_screen_ops *ops, void *data,
		       void (*complete)(void *data, int result));
int cli_application_flush(struct cli_context *ctx);

#endif
