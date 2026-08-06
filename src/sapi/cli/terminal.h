#ifndef CLI_TERMINAL_H
#define CLI_TERMINAL_H

#include <stdio.h>

struct cli_context;

int cli_terminal_init(struct cli_context *ctx, FILE *output, int output_fd);
void cli_terminal_cleanup(struct cli_context *ctx);
void cli_terminal_live_set(struct cli_context *ctx, const char *text);
void cli_terminal_live_clear(struct cli_context *ctx);
void cli_terminal_render_frame(struct cli_context *ctx, int force);
int cli_terminal_next_frame_ms(const struct cli_context *ctx);
void cli_terminal_composer_suspend(struct cli_context *ctx);
void cli_terminal_composer_resume(struct cli_context *ctx);
void cli_terminal_history_begin(struct cli_context *ctx);
void cli_terminal_history_end(struct cli_context *ctx);
void cli_terminal_resize(struct cli_context *ctx);
int cli_terminal_live_active(const struct cli_context *ctx);

#endif
