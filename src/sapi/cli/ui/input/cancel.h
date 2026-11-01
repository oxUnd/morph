#ifndef MORPH_CLI_UI_INPUT_CANCEL_H
#define MORPH_CLI_UI_INPUT_CANCEL_H

struct cli_context;
struct cli_cancel_monitor;

void cli_request_cancel(struct cli_context *ctx);
void cli_signal_received(int number, void *opaque);
struct cli_cancel_monitor *cli_cancel_monitor_start(struct cli_context *ctx, int fd);
void cli_cancel_monitor_stop(struct cli_cancel_monitor *monitor);
void cli_cancel_state_reset(struct cli_context *ctx);
void cli_cancel_monitor_clear(struct cli_context *ctx);

#endif
