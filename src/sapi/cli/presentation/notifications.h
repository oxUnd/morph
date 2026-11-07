#ifndef MORPH_CLI_PRESENTATION_NOTIFICATIONS_H
#define MORPH_CLI_PRESENTATION_NOTIFICATIONS_H

struct cli_application;
struct cli_event_queue_item;
void cli_notifications_render(struct cli_application *ctx, const struct cli_event_queue_item *item);

#endif
