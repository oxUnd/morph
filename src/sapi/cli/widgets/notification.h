#ifndef MORPH_CLI_WIDGETS_NOTIFICATION_H
#define MORPH_CLI_WIDGETS_NOTIFICATION_H

struct cli_application;
struct cli_event_queue_item;

void cli_notification_write(struct cli_application *ctx, const struct cli_event_queue_item *item);

#endif
