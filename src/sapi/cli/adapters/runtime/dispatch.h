#ifndef MORPH_CLI_ADAPTERS_RUNTIME_DISPATCH_H
#define MORPH_CLI_ADAPTERS_RUNTIME_DISPATCH_H

struct cli_application;
struct cli_event_queue_item;
int cli_dispatch_event(struct cli_application *ctx, const struct cli_event_queue_item *item);

#endif
